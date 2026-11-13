const int PINO_TRIG       = 5;
const int PINO_ECHO       = 18;
const int PINO_POT_VAZAO  = 34;
const int PINO_POT_HORA   = 35;
const int PINO_LED_R      = 23;
const int PINO_LED_G      = 22;
const int PINO_LED_B      = 21;
const int PINO_BUZZER     = 19;

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
}