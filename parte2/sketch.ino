/*
 * ============================================================================
 *  CardioIA - Fase 3 | PARTE 2 - Transmissão para nuvem via MQTT (FOG/CLOUD)
 * ============================================================================
 *  Wearable cardíaco simulado no ESP32 (Wokwi).
 *
 *  Sensores (2 sensores distintos):
 *    1) DHT22 ............ temperatura corporal (°C) + umidade relativa (%)
 *    2) Botão de pulso ... cada clique = 1 batimento -> cálculo de BPM
 *
 *  Atuadores / interface:
 *    - Chave deslizante (slide switch) .. simula queda de rede
 *                                         (OFF derruba a sessão MQTT)
 *    - LED verde ......................... indica "online"
 *    - LED vermelho ...................... indica alerta clínico
 *
 *  Lógica de Edge Computing:
 *    - Lê os sensores a cada INTERVALO_AMOSTRA_MS.
 *    - Classifica localmente cada amostra (normal / alerta), sem depender
 *      da nuvem -> o LED de alerta acende mesmo offline.
 *    - Armazena TODAS as amostras num buffer local (fila circular em RAM).
 *    - Quando a rede volta, publica o backlog no broker MQTT
 *      (broker.hivemq.com) em ordem cronológica e apaga o armazenamento local.
 *    - Armazenamento LIMITADO: política de retenção com priorização de
 *      alertas e descarte controlado (ver relatório da Parte 1).
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>              // conexão Wi-Fi do ESP32
#include <PubSubClient.h>      // cliente MQTT (biblioteca "PubSubClient")
#include "DHTesp.h"            // biblioteca "DHT sensor library for ESPx"

// ------------------------- Rede e MQTT --------------------------------------
// Wi-Fi aberto do simulador Wokwi (dá acesso à internet de verdade).
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_SENHA  = "";
// Broker público da HiveMQ: sem login e sem TLS -> só para dados simulados.
// Em produção: HiveMQ Cloud na porta 8883 (TLS) com usuário/senha por device.
const char* MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORTA = 1883;
// Tópico no formato organização/projeto/fase/grupo/recurso. O sufixo "g7k3"
// evita colisão com outros grupos que usem o mesmo broker público.
const char* MQTT_TOPICO = "fiap/cardioia/fase3/g7k3/sinais";
const unsigned long INTERVALO_RECONEXAO_MS = 5000;  // espera entre tentativas

// ---------------------------- Pinagem --------------------------------------
const int PIN_DHT     = 15;   // DATA do DHT22
const int PIN_PULSO   = 4;    // botão (batimento) -> GND, INPUT_PULLUP
const int PIN_WIFI_SW = 27;   // slide switch: HIGH = rede disponível
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
// ~320 kB).
const uint32_t MAX_AMOSTRAS = 2880;
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
WiFiClient wifiClient;          // socket TCP usado pelo cliente MQTT
PubSubClient mqtt(wifiClient);  // sessão MQTT com o broker

Amostra fila[MAX_AMOSTRAS];   // fila circular (buffer offline em RAM)
uint32_t cabeca = 0;          // próxima posição de escrita
uint32_t cauda  = 0;          // próxima amostra a enviar
uint32_t qtdArmazenada = 0;   // amostras pendentes de envio
uint32_t seqGlobal = 0;
uint32_t descartadas = 0;     // amostras perdidas por buffer cheio
uint32_t decimadas = 0;       // amostras normais puladas por decimação

bool wifiConectado = false;   // <- VARIÁVEL BOOLEANA exigida no enunciado
bool wifiAnterior  = false;
unsigned long ultimaAmostra = 0;
// Estado da reconexão MQTT. São "volatile" porque também são alterados pela
// tarefa de conexão, que roda em paralelo com o loop() (ver tarefaConexaoMqtt).
volatile unsigned long ultimaTentativaMqtt = 0;
char mqttId[24];                       // client ID aleatório da sessão atual
volatile bool conectandoMqtt = false;  // true enquanto a tarefa tenta conectar

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

// Payload JSON publicado no tópico MQTT (~100 bytes), por exemplo:
//   {"seq":41,"t_ms":82187,"envio_ms":99341,"temp":36.8,"umid":55,"bpm":79,"alerta":0}
// envio_ms - t_ms = quanto tempo a amostra ficou no buffer offline. O Node-RED
// usa essa diferença para desenhar o ponto no horário em que foi medido.
String amostraParaJSON(const Amostra& s) {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"seq\":%lu,\"t_ms\":%lu,\"envio_ms\":%lu,\"temp\":%.1f,"
           "\"umid\":%u,\"bpm\":%u,\"alerta\":%u}",
           (unsigned long)s.seq, (unsigned long)s.t_ms, (unsigned long)millis(),
           s.temp_x10 / 10.0, s.umid, s.bpm, s.alerta);
  return String(buf);
}

// ============================================================================
//  ARMAZENAMENTO LOCAL (fila circular em RAM)
// ============================================================================
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

void armazenarLocal(const Amostra& s) {
  // Política de decimação quando o buffer está acima de 80%
  float ocupacao = (float)qtdArmazenada / MAX_AMOSTRAS;
  if (ocupacao >= LIMIAR_DECIMACAO && s.alerta == 0 && (s.seq % 2 == 1)) {
    decimadas++;
    return;
  }
  if (qtdArmazenada >= MAX_AMOSTRAS) liberarEspaco();  // buffer cheio
  fila[cabeca] = s;
  cabeca = (cabeca + 1) % MAX_AMOSTRAS;
  qtdArmazenada++;
}

// ============================================================================
//  SINCRONIZAÇÃO com a nuvem (publish MQTT) e limpeza do armazenamento
// ============================================================================
void sincronizarComNuvem() {
  if (qtdArmazenada == 0) return;

  // Em RAM: envia em lotes, para não travar a coleta (o loop continua lendo
  // sensores entre um lote e outro). Cada amostra só sai da fila depois de
  // "enviada" -> se a rede cair no meio, nada se perde.
  static bool emSync = false;
  if (!emSync) {
    Serial.printf("[MQTT] >>> Sincronizando backlog de %lu amostras\n",
                  (unsigned long)qtdArmazenada);
    emSync = true;
  }
  uint16_t enviados = 0;
  while (qtdArmazenada > 0 && enviados < TAM_LOTE_SYNC && wifiConectado) {
    String json = amostraParaJSON(fila[cauda]);
    // QoS 0 (PubSubClient): se o publish falhar, a amostra fica na fila e
    // será reenviada no próximo ciclo -> a garantia de entrega é do firmware.
    if (!mqtt.publish(MQTT_TOPICO, json.c_str())) break;
    Serial.println("[MQTT] " + json);
    cauda = (cauda + 1) % MAX_AMOSTRAS;   // "apaga" a amostra enviada
    qtdArmazenada--;
    enviados++;
  }
  if (qtdArmazenada == 0) {
    cabeca = cauda = 0;                   // buffer local zerado
    Serial.println(F("[MQTT] <<< Sincronizacao concluida. Buffer local apagado."));
    emSync = false;
  }
}

// ============================================================================
//  Conexão MQTT em tarefa separada (FreeRTOS)
//  mqtt.connect() é bloqueante: com o broker fora do ar, cada tentativa pode
//  travar ~10 s. Rodando aqui, fora do loop(), a coleta de sinais continua
//  normalmente enquanto a conexão é negociada. Enquanto conectandoMqtt == true
//  o loop() não toca no objeto mqtt, então as duas tarefas não disputam o
//  mesmo cliente.
// ============================================================================
void tarefaConexaoMqtt(void*) {
  Serial.printf("[MQTT] Conectando em %s como %s...\n", MQTT_BROKER, mqttId);
  if (!mqtt.connect(mqttId))
    Serial.printf("[MQTT] Falha (rc=%d). Nova tentativa em 5 s\n", mqtt.state());
  ultimaTentativaMqtt = millis();   // conta os 5 s a partir do fim da tentativa
  conectandoMqtt = false;           // devolve o cliente MQTT para o loop()
  vTaskDelete(NULL);                // a tarefa termina após cada tentativa
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== CardioIA | Wearable MQTT v2.0 ==="));

  pinMode(PIN_PULSO, INPUT_PULLUP);
  pinMode(PIN_WIFI_SW, INPUT_PULLUP);
  pinMode(PIN_LED_ON, OUTPUT);
  pinMode(PIN_LED_ALR, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSO), isrPulso, FALLING);

  dht.setup(PIN_DHT, DHTesp::DHT22);

  // Wi-Fi não bloqueante: o ESP32 conecta (e reconecta) sozinho em segundo
  // plano, enquanto a coleta já começa. Canal 6 acelera a conexão no Wokwi.
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);

  Serial.printf("Capacidade do buffer offline: %lu amostras\n",
                (unsigned long)MAX_AMOSTRAS);
  Serial.printf("Broker: %s:%u | Topico: %s\n", MQTT_BROKER, MQTT_PORTA, MQTT_TOPICO);
  Serial.println(F("Clique no botao verde no ritmo do coracao para gerar BPM."));
}

// ============================================================================
void loop() {
  unsigned long agora = millis();

  // 1) Estado da conectividade (chave deslizante + Wi-Fi + sessão MQTT)
  bool redeLiberada = (digitalRead(PIN_WIFI_SW) == HIGH);  // chave = "rede"
  if (!conectandoMqtt) {
    // Chave em OFF simula queda de rede: encerra a sessão MQTT.
    if (!redeLiberada && mqtt.connected()) mqtt.disconnect();
    // Rede disponível e sem sessão: dispara uma tentativa de conexão em
    // segundo plano (no máximo uma a cada INTERVALO_RECONEXAO_MS).
    if (redeLiberada && WiFi.status() == WL_CONNECTED && !mqtt.connected() &&
        agora - ultimaTentativaMqtt >= INTERVALO_RECONEXAO_MS) {
      // client ID aleatório: no broker público, dois clientes com o mesmo ID
      // derrubariam um ao outro.
      snprintf(mqttId, sizeof(mqttId), "cardioia-%08lx", (unsigned long)esp_random());
      conectandoMqtt = true;
      xTaskCreate(tarefaConexaoMqtt, "mqtt", 6144, NULL, 1, NULL);
    } else {
      mqtt.loop();   // mantém a sessão viva (keep-alive) e processa a rede
    }
  }

  // "Online" = chave ligada + sessão MQTT ativa (e nenhuma conexão em curso).
  wifiConectado = !conectandoMqtt && redeLiberada && mqtt.connected();
  digitalWrite(PIN_LED_ON, wifiConectado);
  if (wifiConectado != wifiAnterior) {
    Serial.println(wifiConectado ? F("[REDE] ONLINE -> MQTT conectado")
                                 : F("[REDE] OFFLINE -> armazenando localmente"));
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

  // 5) Se online, publica o buffer no broker MQTT e apaga o armazenamento local
  if (wifiConectado) sincronizarComNuvem();

  delay(10);
}
