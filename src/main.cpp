#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

const int PINO_TRIG       = 5;
const int PINO_ECHO       = 18;
const int PINO_POT_VAZAO  = 34;
const int PINO_POT_HORA   = 35;
const int PINO_LED_R      = 23;
const int PINO_LED_G      = 22;
const int PINO_LED_B      = 21;
const int PINO_BUZZER     = 19;

const float DIST_CHEIA_CM       = 20.0;
const float DIST_VAZIA_CM       = 120.0;
const float DIST_MIN_VALIDA_CM  = 2.0;
const float DIST_MAX_VALIDA_CM  = 150.0;

const float NIVEL_VAZIA_PCT        = 10.0;
const float NIVEL_SAI_VAZIA_PCT    = 15.0;

const float VAZAO_ALTA_LPM         = 12.0;

const float VAZAO_MIN_VAZAMENTO    = 0.5;
const int   HORA_INICIO_MADRUGADA  = 0;
const int   HORA_FIM_MADRUGADA     = 5;

const unsigned long TEMPO_CONFIRMA_VAZAMENTO_MS = 5000;

const float VAZAO_MAX_LPM = 30.0;

const int MAX_LEITURAS_INVALIDAS = 3;

const unsigned long INTERVALO_LEITURA_MS   = 500;
const unsigned long INTERVALO_ENVIO_MS     = 5000;
const unsigned long INTERVALO_RECONEXAO_MS = 10000;

const int TAMANHO_MEDIA = 5;

const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_SENHA    = "";
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORTA    = 1883;
const char* TOPICO_ATTRS  = "/TEF/caixa_g01/attrs";

enum Estado {
  NORMAL,
  CONSUMO_ALTO,
  POSSIVEL_VAZAMENTO,
  CAIXA_VAZIA,
  FALHA_SENSOR
};

const char* nomeEstado(Estado e) {
  switch (e) {
    case NORMAL:             return "NORMAL";
    case CONSUMO_ALTO:       return "CONSUMO_ALTO";
    case POSSIVEL_VAZAMENTO: return "POSSIVEL_VAZAMENTO";
    case CAIXA_VAZIA:        return "CAIXA_VAZIA";
    case FALHA_SENSOR:       return "FALHA_SENSOR";
  }
  return "?";
}

WiFiClient   clienteWifi;
PubSubClient mqtt(clienteWifi);

float bufferDistancia[TAMANHO_MEDIA];
float bufferVazao[TAMANHO_MEDIA];
int   posDistancia = 0, qtdDistancia = 0;
int   posVazao = 0,     qtdVazao = 0;

float nivelPct  = 0;
float vazaoLpm  = 0;
int   hora      = 12;
int   leiturasInvalidas = 0;

Estado estadoAtual = NORMAL;
bool   caixaVaziaAtiva = false;
unsigned long inicioVazaoMadrugada = 0;

unsigned long ultimaLeitura   = 0;
unsigned long ultimoEnvio     = 0;
unsigned long ultimaReconexao = 0;

bool buzzerLigado = false;

float lerDistanciaCm() {
  digitalWrite(PINO_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);

  unsigned long duracao = pulseIn(PINO_ECHO, HIGH, 30000);
  if (duracao == 0) return -1;

  float distancia = duracao * 0.0343 / 2.0;

  if (distancia < DIST_MIN_VALIDA_CM || distancia > DIST_MAX_VALIDA_CM) {
    return -1;
  }
  return distancia;
}

float lerVazaoLpm() {
  int bruto = analogRead(PINO_POT_VAZAO);
  return bruto * VAZAO_MAX_LPM / 4095.0;
}

int lerHora() {
  int bruto = analogRead(PINO_POT_HORA);
  return constrain(map(bruto, 0, 4095, 0, 23), 0, 23);
}

float mediaMovel(float buffer[], int &pos, int &qtd, float novoValor) {
  buffer[pos] = novoValor;
  pos = (pos + 1) % TAMANHO_MEDIA;
  if (qtd < TAMANHO_MEDIA) qtd++;

  float soma = 0;
  for (int i = 0; i < qtd; i++) soma += buffer[i];
  return soma / qtd;
}

float distanciaParaNivel(float distancia) {
  float nivel = (DIST_VAZIA_CM - distancia) / (DIST_VAZIA_CM - DIST_CHEIA_CM) * 100.0;
  return constrain(nivel, 0.0, 100.0);
}

bool ehMadrugada(int h) {
  return h >= HORA_INICIO_MADRUGADA && h < HORA_FIM_MADRUGADA;
}

void atualizarLeituras() {
  float distancia = lerDistanciaCm();

  if (distancia < 0) {
    leiturasInvalidas++;
  } else {
    leiturasInvalidas = 0;
    float distSuavizada = mediaMovel(bufferDistancia, posDistancia, qtdDistancia, distancia);
    nivelPct = distanciaParaNivel(distSuavizada);
  }

  vazaoLpm = mediaMovel(bufferVazao, posVazao, qtdVazao, lerVazaoLpm());
  hora     = lerHora();
}

Estado decidirEstado() {
  if (leiturasInvalidas >= MAX_LEITURAS_INVALIDAS) {
    return FALHA_SENSOR;
  }

  if (nivelPct < NIVEL_VAZIA_PCT)          caixaVaziaAtiva = true;
  else if (nivelPct > NIVEL_SAI_VAZIA_PCT) caixaVaziaAtiva = false;
  if (caixaVaziaAtiva) return CAIXA_VAZIA;

  if (ehMadrugada(hora) && vazaoLpm > VAZAO_MIN_VAZAMENTO) {
    if (inicioVazaoMadrugada == 0) inicioVazaoMadrugada = millis();
    if (millis() - inicioVazaoMadrugada >= TEMPO_CONFIRMA_VAZAMENTO_MS) {
      return POSSIVEL_VAZAMENTO;
    }
  } else {
    inicioVazaoMadrugada = 0;
  }

  if (vazaoLpm > VAZAO_ALTA_LPM) return CONSUMO_ALTO;

  return NORMAL;
}

void definirCor(bool r, bool g, bool b) {
  digitalWrite(PINO_LED_R, r);
  digitalWrite(PINO_LED_G, g);
  digitalWrite(PINO_LED_B, b);
}

void buzzer(bool ligar, int frequencia) {
  if (ligar && !buzzerLigado) {
    tone(PINO_BUZZER, frequencia);
    buzzerLigado = true;
  } else if (!ligar && buzzerLigado) {
    noTone(PINO_BUZZER);
    buzzerLigado = false;
  }
}

void atualizarAlertas(Estado e) {
  unsigned long agora = millis();

  switch (e) {
    case NORMAL:
      definirCor(LOW, HIGH, LOW);
      buzzer(false, 0);
      break;

    case CONSUMO_ALTO:
      definirCor(HIGH, HIGH, LOW);
      buzzer(agora % 4000 < 150, 1000);
      break;

    case POSSIVEL_VAZAMENTO: {
      bool fase = (agora / 500) % 2 == 0;
      definirCor(fase, LOW, LOW);
      buzzer(fase, 1500);
      break;
    }

    case CAIXA_VAZIA:
      definirCor(HIGH, LOW, LOW);
      buzzer(true, 800);
      break;

    case FALHA_SENSOR: {
      bool fase = (agora / 1000) % 2 == 0;
      definirCor(LOW, LOW, fase);
      unsigned long t = agora % 5000;
      buzzer(t < 100 || (t > 250 && t < 350), 2000);
      break;
    }
  }
}

void iniciarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setSocketTimeout(2);
}

void manterConexao() {
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (millis() - ultimaReconexao < INTERVALO_RECONEXAO_MS) return;
  ultimaReconexao = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[REDE] Sem Wi-Fi - operando apenas localmente");
    return;
  }

  String idCliente = "caixa-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(idCliente.c_str())) {
    Serial.println("[REDE] MQTT conectado");
  } else {
    Serial.print("[REDE] Falha no MQTT, código ");
    Serial.println(mqtt.state());
  }
}

void publicar() {
  char mensagem[96];
  snprintf(mensagem, sizeof(mensagem), "n|%.1f|v|%.1f|h|%d|s|%s",
           nivelPct, vazaoLpm, hora, nomeEstado(estadoAtual));

  if (mqtt.connected()) {
    mqtt.publish(TOPICO_ATTRS, mensagem);
    Serial.print("[MQTT] Enviado: ");
  } else {
    Serial.print("[MQTT] Offline, não enviado: ");
  }
  Serial.println(mensagem);
}

void imprimirStatus() {
  Serial.printf("[%02dh] Nivel: %5.1f%% | Vazao: %4.1f L/min | Estado: %s",
                hora, nivelPct, vazaoLpm, nomeEstado(estadoAtual));
  if (leiturasInvalidas > 0) {
    Serial.printf(" | leituras invalidas seguidas: %d", leiturasInvalidas);
  }
  if (inicioVazaoMadrugada != 0 && estadoAtual != POSSIVEL_VAZAMENTO) {
    Serial.printf(" | vazao na madrugada ha %lu s",
                  (millis() - inicioVazaoMadrugada) / 1000);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  pinMode(PINO_TRIG, OUTPUT);
  pinMode(PINO_ECHO, INPUT);
  pinMode(PINO_LED_R, OUTPUT);
  pinMode(PINO_LED_G, OUTPUT);
  pinMode(PINO_LED_B, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);

  Serial.println("=== SEP System - iniciando ===");
  iniciarWifi();
}

void loop() {
  unsigned long agora = millis();

  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    atualizarLeituras();

    Estado novoEstado = decidirEstado();
    if (novoEstado != estadoAtual) {
      Serial.printf(">>> Mudanca de estado: %s -> %s\n",
                    nomeEstado(estadoAtual), nomeEstado(novoEstado));
      estadoAtual = novoEstado;
      publicar();
    }
    imprimirStatus();
  }

  atualizarAlertas(estadoAtual);

  manterConexao();
  if (agora - ultimoEnvio >= INTERVALO_ENVIO_MS) {
    ultimoEnvio = agora;
    publicar();
  }
}