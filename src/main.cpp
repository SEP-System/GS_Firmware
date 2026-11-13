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

const float VAZAO_MAX_LPM = 30.0;

const unsigned long INTERVALO_LEITURA_MS   = 500;

float nivelPct  = 0;
float vazaoLpm  = 0;
int   hora      = 12;

unsigned long ultimaLeitura   = 0;

float lerDistanciaCm() {
  digitalWrite(PINO_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);

  unsigned long duracao = pulseIn(PINO_ECHO, HIGH, 30000);
  if (duracao == 0) return -1;

  return duracao * 0.0343 / 2.0;
}

float lerVazaoLpm() {
  int bruto = analogRead(PINO_POT_VAZAO);
  return bruto * VAZAO_MAX_LPM / 4095.0;
}

int lerHora() {
  int bruto = analogRead(PINO_POT_HORA);
  return constrain(map(bruto, 0, 4095, 0, 23), 0, 23);
}

float distanciaParaNivel(float distancia) {
  float nivel = (DIST_VAZIA_CM - distancia) / (DIST_VAZIA_CM - DIST_CHEIA_CM) * 100.0;
  return constrain(nivel, 0.0, 100.0);
}

void atualizarLeituras() {
  float distancia = lerDistanciaCm();
  if (distancia >= 0) {
    nivelPct = distanciaParaNivel(distancia);
  }
  vazaoLpm = lerVazaoLpm();
  hora     = lerHora();
}

void imprimirStatus() {
  Serial.printf("[%02dh] Nivel: %5.1f%% | Vazao: %4.1f L/min\n",
                hora, nivelPct, vazaoLpm);
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