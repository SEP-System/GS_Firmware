#include <Arduino.h>

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

const float VAZAO_MAX_LPM = 30.0;

const int MAX_LEITURAS_INVALIDAS = 3;

const unsigned long INTERVALO_LEITURA_MS   = 500;

const int TAMANHO_MEDIA = 5;

float bufferDistancia[TAMANHO_MEDIA];
float bufferVazao[TAMANHO_MEDIA];
int   posDistancia = 0, qtdDistancia = 0;
int   posVazao = 0,     qtdVazao = 0;

float nivelPct  = 0;
float vazaoLpm  = 0;
int   hora      = 12;
int   leiturasInvalidas = 0;

unsigned long ultimaLeitura   = 0;

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

void imprimirStatus() {
  Serial.printf("[%02dh] Nivel: %5.1f%% | Vazao: %4.1f L/min",
                hora, nivelPct, vazaoLpm);
  if (leiturasInvalidas > 0) {
    Serial.printf(" | leituras invalidas seguidas: %d", leiturasInvalidas);
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
}

void loop() {
  unsigned long agora = millis();

  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    atualizarLeituras();
    imprimirStatus();
  }
}