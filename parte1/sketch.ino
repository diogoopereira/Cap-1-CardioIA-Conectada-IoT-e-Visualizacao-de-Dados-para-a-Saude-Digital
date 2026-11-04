/*
 * ============================================================================
 *  CardioIA - Fase 3 | PARTE 1 - Armazenamento e processamento local (EDGE)
 * ============================================================================
 *  Wearable cardíaco simulado no ESP32 (Wokwi).
 *
 *  Sensores (2 sensores distintos):
 *    1) DHT22 ............ temperatura corporal (°C) + umidade relativa (%)
 *    2) Botão de pulso ... cada clique = 1 batimento -> cálculo de BPM
 *
 *  Atuadores / interface:
 *    - Chave deslizante (slide switch) .. simula a conectividade Wi-Fi
 *                                         (variável booleana wifiConectado)
 *    - LED verde ......................... indica "online"
 *    - LED vermelho ...................... indica alerta clínico
 *
 *  Lógica de Edge Computing:
 *    - Lê os sensores a cada INTERVALO_AMOSTRA_MS.
 *    - Classifica localmente cada amostra (normal / alerta), sem depender
 *      da nuvem -> o LED de alerta acende mesmo offline.
 *    - Armazena TODAS as amostras num buffer local (fila circular em RAM,
 *      ou SPIFFS em chip real, ativando USE_SPIFFS = 1).
 *    - Quando a "rede" volta, sincroniza o backlog (Serial.println = "nuvem")
 *      em ordem cronológica e apaga o armazenamento local.
 *    - Armazenamento LIMITADO: política de retenção com priorização de
 *      alertas e descarte controlado (ver relatório).
 * ============================================================================
 */

#include <Arduino.h>
#include "DHTesp.h"            // biblioteca "DHT sensor library for ESPx"

// ---------------------------------------------------------------------------
// [OPCIONAL] SPIFFS - só persiste em ESP32 FÍSICO. No Wokwi deixe 0.
// ---------------------------------------------------------------------------
#define USE_SPIFFS 0
#if USE_SPIFFS
  #include "FS.h"
  #include "SPIFFS.h"
  const char* ARQ_DADOS = "/leituras.csv";
#endif

// ---------------------------- Pinagem --------------------------------------
const int PIN_DHT     = 15;   // DATA do DHT22
const int PIN_PULSO   = 4;    // botão (batimento) -> GND, INPUT_PULLUP
const int PIN_WIFI_SW = 27;   // slide switch: HIGH = Wi-Fi conectado
const int PIN_LED_ON  = 26;   // LED verde (online)
const int PIN_LED_ALR = 2;    // LED vermelho (alerta)

// ------------------------ Parâmetros clínicos -------------------------------
// Limites definidos pelo grupo (adulto em repouso).
const float TEMP_FEBRE      = 38.0;   // °C  -> febre
const float TEMP_HIPOTERMIA = 35.0;   // °C  -> hipotermia
const int   BPM_TAQUI       = 120;    // bpm -> taquicardia
const int   BPM_BRADI       = 50;     // bpm -> bradicardia (só se houver pulso)

// ---------------------- Parâmetros de amostragem ----------------------------
const unsigned long INTERVALO_AMOSTRA_MS = 2000;  // 1 amostra a cada 2 s (demo)
                                                  // produção: 5 s
const unsigned long JANELA_BPM_MS        = 15000; // janela p/ cálculo de BPM
const unsigned long DEBOUNCE_MS          = 250;   // máx. 240 bpm

// ---------------------- Estratégia de armazenamento -------------------------
// Modelo de negócio: paciente cardiológico em monitoramento domiciliar.
// Em produção (1 amostra / 5 s) o requisito é aguentar 4 h sem rede:
//   4 h * 3600 s / 5 s = 2.880 amostras.
// Cada amostra ocupa 16 bytes -> 2.880 * 16 B ≈ 46 kB de RAM (o ESP32 tem
// ~320 kB). Em SPIFFS (~1,4 MB úteis, ~40 B/linha CSV) caberiam ~35.000
// amostras (≈ 48 h), por isso em chip real o limite sobe para 17.280 (24 h).
#if USE_SPIFFS
  const uint32_t MAX_AMOSTRAS = 17280;
#else
  const uint32_t MAX_AMOSTRAS = 2880;
#endif
// Acima deste nível de ocupação, amostras NORMAIS são decimadas (guarda 1 de
// cada 2), mas amostras de ALERTA são sempre guardadas.
const float LIMIAR_DECIMACAO = 0.80;
const uint16_t TAM_LOTE_SYNC = 20;   // amostras enviadas por ciclo de loop()

// ------------------------- Estrutura da amostra -----------------------------
struct Amostra {
  uint32_t seq;        // nº sequencial (detecta perdas na nuvem)
  uint32_t t_ms;       // instante da coleta (millis)
  int16_t  temp_x10;   // temperatura * 10 (ex.: 368 = 36,8 °C)
  uint8_t  umid;       // umidade %
  uint8_t  bpm;        // batimentos por minuto
  uint8_t  alerta;     // bitmask de alertas (ver ALR_*)
  uint8_t  _pad[3];
};

enum : uint8_t {
  ALR_FEBRE = 0x01, ALR_HIPOTERMIA = 0x02,
  ALR_TAQUI = 0x04, ALR_BRADI = 0x08, ALR_SEM_PULSO = 0x10, ALR_SENSOR = 0x20
};

// --------------------------- Estado global ----------------------------------
DHTesp dht;

#if !USE_SPIFFS
Amostra fila[MAX_AMOSTRAS];   // fila circular (buffer offline em RAM)
uint32_t cabeca = 0;          // próxima posição de escrita
uint32_t cauda  = 0;          // próxima amostra a enviar
#endif
uint32_t qtdArmazenada = 0;   // amostras pendentes de envio
uint32_t seqGlobal = 0;
uint32_t descartadas = 0;     // amostras perdidas por buffer cheio
uint32_t decimadas = 0;       // amostras normais puladas por decimação

bool wifiConectado = false;   // <- VARIÁVEL BOOLEANA exigida no enunciado
bool wifiAnterior  = false;
unsigned long ultimaAmostra = 0;

// Batimentos registrados pela interrupção do botão
volatile unsigned long batidas[32];
volatile uint8_t idxBatida = 0;
volatile unsigned long ultimaBatida = 0;

// ============================================================================
//  ISR do botão de pulso: registra o instante de cada batimento
// ============================================================================
void IRAM_ATTR isrPulso() {
  unsigned long agora = millis();
  if (agora - ultimaBatida < DEBOUNCE_MS) return;  // debounce
  ultimaBatida = agora;
  batidas[idxBatida] = agora;
  idxBatida = (idxBatida + 1) % 32;
}

// ============================================================================
//  Calcula BPM com base nos batimentos dentro da janela de 15 s
//  BPM = 60.000 * (n-1) / (t_ultimo - t_primeiro)
// ============================================================================
int calcularBPM() {
  unsigned long agora = millis();
  unsigned long primeiro = 0, ultimo = 0;
  int n = 0;
  noInterrupts();
  for (int i = 0; i < 32; i++) {
    unsigned long t = batidas[i];
    if (t != 0 && agora - t <= JANELA_BPM_MS) {
      if (n == 0 || t < primeiro) primeiro = t;
      if (n == 0 || t > ultimo)   ultimo = t;
      n++;
    }
  }
  interrupts();
  if (n < 2 || ultimo == primeiro) return 0;          // sem pulso detectado
  if (agora - ultimo > 5000) return 0;                // parou de bater há 5 s
  long bpm = 60000L * (n - 1) / (long)(ultimo - primeiro);
  return constrain(bpm, 0, 250);
}

// ============================================================================
//  Classificação local (Edge) -> bitmask de alertas
// ============================================================================
uint8_t avaliarRisco(float temp, int bpm, bool sensorOk) {
  uint8_t a = 0;
  if (!sensorOk)               a |= ALR_SENSOR;
  else {
    if (temp >= TEMP_FEBRE)      a |= ALR_FEBRE;
    if (temp <  TEMP_HIPOTERMIA) a |= ALR_HIPOTERMIA;
  }
  if (bpm == 0)                a |= ALR_SEM_PULSO;
  else if (bpm > BPM_TAQUI)    a |= ALR_TAQUI;
  else if (bpm < BPM_BRADI)    a |= ALR_BRADI;
  return a;
}

String descreverAlerta(uint8_t a) {
  if (a == 0) return "NORMAL";
  String s;
  if (a & ALR_FEBRE)      s += "FEBRE ";
  if (a & ALR_HIPOTERMIA) s += "HIPOTERMIA ";
  if (a & ALR_TAQUI)      s += "TAQUICARDIA ";
  if (a & ALR_BRADI)      s += "BRADICARDIA ";
  if (a & ALR_SEM_PULSO)  s += "SEM_PULSO ";
  if (a & ALR_SENSOR)     s += "FALHA_SENSOR ";
  s.trim();
  return s;
}

// Linha CSV padronizada: seq,t_ms,temp,umid,bpm,alerta
String amostraParaCSV(const Amostra& s) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%lu,%lu,%.1f,%u,%u,%u",
           (unsigned long)s.seq, (unsigned long)s.t_ms,
           s.temp_x10 / 10.0, s.umid, s.bpm, s.alerta);
  return String(buf);
}

// ============================================================================
//  ARMAZENAMENTO LOCAL (fila circular em RAM  |  arquivo CSV no SPIFFS)
// ============================================================================
#if !USE_SPIFFS
// Remove a amostra mais antiga, preferindo descartar uma amostra NORMAL
// (as primeiras 50 posições da cauda são varridas em busca de uma normal).
void liberarEspaco() {
  // caso simples: descarta a mais antiga
  for (uint32_t k = 0; k < 50 && k < qtdArmazenada; k++) {
    uint32_t pos = (cauda + k) % MAX_AMOSTRAS;
    if (fila[pos].alerta == 0) {
      // "puxa" as k amostras anteriores uma posição para frente
      for (uint32_t j = k; j > 0; j--) {
        fila[(cauda + j) % MAX_AMOSTRAS] = fila[(cauda + j - 1) % MAX_AMOSTRAS];
      }
      break;
    }
  }
  cauda = (cauda + 1) % MAX_AMOSTRAS;
  qtdArmazenada--;
  descartadas++;
}
#endif

void armazenarLocal(const Amostra& s) {
  // Política de decimação quando o buffer está acima de 80%
  float ocupacao = (float)qtdArmazenada / MAX_AMOSTRAS;
  if (ocupacao >= LIMIAR_DECIMACAO && s.alerta == 0 && (s.seq % 2 == 1)) {
    decimadas++;
    return;
  }
#if USE_SPIFFS
  if (qtdArmazenada >= MAX_AMOSTRAS) { descartadas++; return; }
  File f = SPIFFS.open(ARQ_DADOS, FILE_APPEND);
  if (f) { f.println(amostraParaCSV(s)); f.close(); qtdArmazenada++; }
#else
  if (qtdArmazenada >= MAX_AMOSTRAS) liberarEspaco();  // buffer cheio
  fila[cabeca] = s;
  cabeca = (cabeca + 1) % MAX_AMOSTRAS;
  qtdArmazenada++;
#endif
}

// ============================================================================
//  SINCRONIZAÇÃO com a "nuvem" (Serial.println) e limpeza do armazenamento
// ============================================================================
void sincronizarComNuvem() {
  if (qtdArmazenada == 0) return;

#if USE_SPIFFS
  // Em chip real: envia o arquivo inteiro e depois o apaga.
  File f = SPIFFS.open(ARQ_DADOS, FILE_READ);
  if (!f) return;
  Serial.println(F("[NUVEM] >>> Inicio da sincronizacao (SPIFFS)"));
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length()) Serial.println("[NUVEM] " + linha);
  }
  f.close();
  SPIFFS.remove(ARQ_DADOS);                 // apaga o arquivo local
  Serial.printf("[NUVEM] <<< %lu amostras enviadas. Arquivo local apagado.\n",
                (unsigned long)qtdArmazenada);
  qtdArmazenada = 0;
#else
  // Em RAM: envia em lotes, para não travar a coleta (o loop continua lendo
  // sensores entre um lote e outro). Cada amostra só sai da fila depois de
  // "enviada" -> se a rede cair no meio, nada se perde.
  static bool emSync = false;
  if (!emSync) {
    Serial.printf("[NUVEM] >>> Sincronizando backlog de %lu amostras\n",
                  (unsigned long)qtdArmazenada);
    emSync = true;
  }
  uint16_t enviados = 0;
  while (qtdArmazenada > 0 && enviados < TAM_LOTE_SYNC && wifiConectado) {
    Serial.println("[NUVEM] " + amostraParaCSV(fila[cauda]));
    cauda = (cauda + 1) % MAX_AMOSTRAS;   // "apaga" a amostra enviada
    qtdArmazenada--;
    enviados++;
  }
  if (qtdArmazenada == 0) {
    cabeca = cauda = 0;                   // buffer local zerado
    Serial.println(F("[NUVEM] <<< Sincronizacao concluida. Buffer local apagado."));
    emSync = false;
  }
#endif
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== CardioIA | Wearable Edge v1.0 ==="));

  pinMode(PIN_PULSO, INPUT_PULLUP);
  pinMode(PIN_WIFI_SW, INPUT_PULLUP);
  pinMode(PIN_LED_ON, OUTPUT);
  pinMode(PIN_LED_ALR, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSO), isrPulso, FALLING);

  dht.setup(PIN_DHT, DHTesp::DHT22);

#if USE_SPIFFS
  if (!SPIFFS.begin(true)) Serial.println(F("[ERRO] Falha ao montar SPIFFS"));
  else {
    // conta linhas já existentes (dados de antes de um reboot sobrevivem!)
    File f = SPIFFS.open(ARQ_DADOS, FILE_READ);
    while (f && f.available()) { f.readStringUntil('\n'); qtdArmazenada++; }
    if (f) f.close();
    Serial.printf("[SPIFFS] %lu amostras pendentes recuperadas\n",
                  (unsigned long)qtdArmazenada);
  }
#endif
  Serial.printf("Capacidade do buffer offline: %lu amostras\n",
                (unsigned long)MAX_AMOSTRAS);
  Serial.println(F("CSV: seq,t_ms,temp,umid,bpm,alerta"));
  Serial.println(F("Clique no botao verde no ritmo do coracao para gerar BPM."));
}

// ============================================================================
void loop() {
  unsigned long agora = millis();

  // 1) Estado da conectividade (simulada pela chave deslizante)
  wifiConectado = (digitalRead(PIN_WIFI_SW) == HIGH);
  digitalWrite(PIN_LED_ON, wifiConectado);
  if (wifiConectado != wifiAnterior) {
    Serial.println(wifiConectado ? F("[REDE] Wi-Fi CONECTADO")
                                 : F("[REDE] Wi-Fi DESCONECTADO -> modo offline"));
    wifiAnterior = wifiConectado;
  }

  // 2) Coleta periódica (acontece SEMPRE, online ou offline)
  if (agora - ultimaAmostra >= INTERVALO_AMOSTRA_MS) {
    ultimaAmostra = agora;

    TempAndHumidity th = dht.getTempAndHumidity();
    bool sensorOk = (dht.getStatus() == DHTesp::ERROR_NONE) && !isnan(th.temperature);
    int bpm = calcularBPM();

    Amostra s;
    s.seq      = ++seqGlobal;
    s.t_ms     = agora;
    s.temp_x10 = sensorOk ? (int16_t)lroundf(th.temperature * 10) : 0;
    s.umid     = sensorOk ? (uint8_t)constrain(lroundf(th.humidity), 0, 100) : 0;
    s.bpm      = (uint8_t)bpm;
    s.alerta   = avaliarRisco(th.temperature, bpm, sensorOk);

    // 3) Decisão local (Edge): alerta imediato no próprio dispositivo
    digitalWrite(PIN_LED_ALR, s.alerta != 0);

    // 4) Persistência local: toda amostra entra no buffer primeiro
    armazenarLocal(s);

    Serial.printf("[LEITURA] #%lu T=%.1fC U=%u%% BPM=%u -> %s | %s | buffer=%lu/%lu"
                  " (descart=%lu, decim=%lu)\n",
                  (unsigned long)s.seq, s.temp_x10 / 10.0, s.umid, s.bpm,
                  descreverAlerta(s.alerta).c_str(),
                  wifiConectado ? "ONLINE" : "OFFLINE",
                  (unsigned long)qtdArmazenada, (unsigned long)MAX_AMOSTRAS,
                  (unsigned long)descartadas, (unsigned long)decimadas);
  }

  // 5) Se online, drena o buffer para a nuvem e apaga o armazenamento local
  if (wifiConectado) sincronizarComNuvem();

  delay(10);
}
