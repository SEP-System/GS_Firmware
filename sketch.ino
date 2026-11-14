/*
 *  SEP System  -  FIAP Global Solution 2026.2 (CitizenTech)
 *  Edge Computing & Computer Systems
 *
 *  O que o dispositivo faz:
 *    MEDIR   -> nível da caixa (HC-SR04) e vazão de saída (potenciômetro que
 *               simula um sensor de vazão YF-S201, que não existe no Wokwi).
 *    DECIDIR -> classifica a situação em um de 5 estados.
 *    ALERTAR -> LED RGB e buzzer com uma resposta diferente por estado.
 *               Funciona mesmo SEM internet (decisão na borda).
 *    ENVIAR  -> quando há Wi-Fi, publica as leituras via MQTT no formato
 *               UltraLight do IoT Agent FIWARE.
 *
 *  Estados:
 *    NORMAL             -> LED verde fixo, silêncio
 *    CONSUMO_ALTO       -> LED amarelo, bip curto a cada 4 s
 *    POSSIVEL_VAZAMENTO -> LED vermelho piscando, buzzer intermitente
 *    CAIXA_VAZIA        -> LED vermelho fixo, buzzer contínuo
 *    FALHA_SENSOR       -> LED azul piscando, bip duplo a cada 5 s
 *
 *  Simulação no Wokwi:
 *    - Clique no HC-SR04 e mova a distância: perto = caixa cheia,
 *      longe = caixa vazia. Acima de 150 cm = leitura impossível (falha).
 *    - Potenciômetro "VAZÃO": 0 a 30 L/min de água saindo da caixa.
 *    - Potenciômetro "HORA": simula o relógio de 0h a 23h.
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ---------------------------------------------------------------------------
// PINOS
// ---------------------------------------------------------------------------
const int PINO_TRIG       = 5;   // HC-SR04 - disparo
const int PINO_ECHO       = 18;  // HC-SR04 - retorno do eco
const int PINO_POT_VAZAO  = 34;  // simula o sensor de vazão (entrada analógica)
const int PINO_POT_HORA   = 35;  // simula o relógio (entrada analógica)
const int PINO_LED_R      = 23;  // LED RGB - vermelho
const int PINO_LED_G      = 22;  // LED RGB - verde
const int PINO_LED_B      = 21;  // LED RGB - azul
const int PINO_BUZZER     = 19;  // buzzer

// ---------------------------------------------------------------------------
// GEOMETRIA DA CAIXA (caixa de 1000 L com ~100 cm de altura útil)
// O sensor fica na tampa, apontado para baixo: quanto MAIOR a distância,
// MENOR o nível de água.
// ---------------------------------------------------------------------------
const float DIST_CHEIA_CM       = 20.0;   // distância sensor -> água com a caixa cheia
const float DIST_VAZIA_CM       = 120.0;  // distância sensor -> fundo da caixa
const float DIST_MIN_VALIDA_CM  = 2.0;    // HC-SR04 não mede abaixo de 2 cm
const float DIST_MAX_VALIDA_CM  = 150.0;  // além do fundo da caixa = eco errado

// ---------------------------------------------------------------------------
// LIMITES DE DECISÃO (justificativas nos comentários)
// ---------------------------------------------------------------------------
// Abaixo de 10% a caixa só tem água para poucos minutos de uso.
const float NIVEL_VAZIA_PCT        = 10.0;
// Histerese: só sai de "vazia" quando passar de 15%, para o alarme não ficar
// ligando e desligando com a água balançando perto do limite.
const float NIVEL_SAI_VAZIA_PCT    = 15.0;

// Um chuveiro gasta ~6 a 12 L/min. Acima de 12 L/min há vários pontos abertos
// ao mesmo tempo ou um cano rompido.
const float VAZAO_ALTA_LPM         = 12.0;

// Na madrugada quase ninguém usa água. Qualquer saída acima de 0,5 L/min
// (uma torneira pingando forte) por muito tempo indica vazamento.
const float VAZAO_MIN_VAZAMENTO    = 0.5;
const int   HORA_INICIO_MADRUGADA  = 0;   // 0h
const int   HORA_FIM_MADRUGADA     = 5;   // até 4h59

// Na vida real: 30 min de vazão contínua (uma descarga de madrugada não conta).
// Na simulação usamos 5 s para dar tempo de demonstrar no vídeo.
const unsigned long TEMPO_CONFIRMA_VAZAMENTO_MS = 5000;

// Escala do "sensor de vazão" simulado
const float VAZAO_MAX_LPM = 30.0;

// Leituras inválidas seguidas até declarar falha do sensor
const int MAX_LEITURAS_INVALIDAS = 3;

// ---------------------------------------------------------------------------
// TEMPOS (tudo com millis(), sem delay())
// ---------------------------------------------------------------------------
const unsigned long INTERVALO_LEITURA_MS   = 500;    // lê os sensores 2x por segundo
const unsigned long INTERVALO_ENVIO_MS     = 5000;   // publica no MQTT a cada 5 s
const unsigned long INTERVALO_RECONEXAO_MS = 10000;  // tenta reconectar a cada 10 s

// ---------------------------------------------------------------------------
// SUAVIZAÇÃO: média móvel das últimas N leituras
// ---------------------------------------------------------------------------
const int TAMANHO_MEDIA = 5;

// ---------------------------------------------------------------------------
// REDE E MQTT (FIWARE - IoT Agent UltraLight)
// ---------------------------------------------------------------------------
const char* WIFI_SSID     = "Wokwi-GUEST";        // rede aberta do Wokwi
const char* WIFI_SENHA    = "";
const char* MQTT_BROKER   = "54.236.175.117";   // Elastic IP da VM na AWS (Mosquitto do FIWARE)
const int   MQTT_PORTA    = 1883;
// Tópico no formato /<apikey>/<device_id>/attrs. O device_id "caixa_g01"
// deve ser o mesmo usado no provisionamento do dispositivo no IoT Agent (Postman).
const char* TOPICO_ATTRS  = "/TEF/caixa_g01/attrs";

// ===========================================================================
// ESTADOS DO SISTEMA
// ===========================================================================
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

// ---------------------------------------------------------------------------
// VARIÁVEIS GLOBAIS
// ---------------------------------------------------------------------------
WiFiClient   clienteWifi;
PubSubClient mqtt(clienteWifi);

float bufferDistancia[TAMANHO_MEDIA];
float bufferVazao[TAMANHO_MEDIA];
int   posDistancia = 0, qtdDistancia = 0;
int   posVazao = 0,     qtdVazao = 0;

float nivelPct  = 0;     // nível suavizado (%)
float vazaoLpm  = 0;     // vazão suavizada (L/min)
int   hora      = 12;    // hora simulada (0-23)
int   leiturasInvalidas = 0;

Estado estadoAtual = NORMAL;
bool   caixaVaziaAtiva = false;          // memória para a histerese
unsigned long inicioVazaoMadrugada = 0;  // 0 = não está contando

unsigned long ultimaLeitura   = 0;
unsigned long ultimoEnvio     = 0;
unsigned long ultimaReconexao = 0;

bool buzzerLigado = false;

// ===========================================================================
// FUNÇÕES DE LEITURA
// ===========================================================================

// Mede a distância com o HC-SR04. Retorna -1 se a leitura for inválida.
float lerDistanciaCm() {
  digitalWrite(PINO_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);

  // timeout de 30 ms (~5 m): sem eco = leitura inválida
  unsigned long duracao = pulseIn(PINO_ECHO, HIGH, 30000);
  if (duracao == 0) return -1;

  float distancia = duracao * 0.0343 / 2.0;  // velocidade do som: 343 m/s

  if (distancia < DIST_MIN_VALIDA_CM || distancia > DIST_MAX_VALIDA_CM) {
    return -1;  // fora do que é fisicamente possível dentro da caixa
  }
  return distancia;
}

// Lê o potenciômetro e converte para L/min (simula o sensor de vazão)
float lerVazaoLpm() {
  int bruto = analogRead(PINO_POT_VAZAO);  // 0 a 4095
  return bruto * VAZAO_MAX_LPM / 4095.0;
}

// Lê o potenciômetro de hora e converte para 0-23 h
int lerHora() {
  int bruto = analogRead(PINO_POT_HORA);
  return constrain(map(bruto, 0, 4095, 0, 23), 0, 23);
}

// Adiciona um valor ao buffer circular e devolve a média das leituras guardadas
float mediaMovel(float buffer[], int &pos, int &qtd, float novoValor) {
  buffer[pos] = novoValor;
  pos = (pos + 1) % TAMANHO_MEDIA;
  if (qtd < TAMANHO_MEDIA) qtd++;

  float soma = 0;
  for (int i = 0; i < qtd; i++) soma += buffer[i];
  return soma / qtd;
}

// Converte distância (cm) em nível da caixa (%)
float distanciaParaNivel(float distancia) {
  float nivel = (DIST_VAZIA_CM - distancia) / (DIST_VAZIA_CM - DIST_CHEIA_CM) * 100.0;
  return constrain(nivel, 0.0, 100.0);
}

bool ehMadrugada(int h) {
  return h >= HORA_INICIO_MADRUGADA && h < HORA_FIM_MADRUGADA;
}

// Faz todas as leituras, trata leitura inválida e aplica a suavização
void atualizarLeituras() {
  float distancia = lerDistanciaCm();

  if (distancia < 0) {
    leiturasInvalidas++;  // mantém o último nível válido
  } else {
    leiturasInvalidas = 0;
    float distSuavizada = mediaMovel(bufferDistancia, posDistancia, qtdDistancia, distancia);
    nivelPct = distanciaParaNivel(distSuavizada);
  }

  vazaoLpm = mediaMovel(bufferVazao, posVazao, qtdVazao, lerVazaoLpm());
  hora     = lerHora();
}

// ===========================================================================
// DECISÃO
// ===========================================================================
Estado decidirEstado() {
  // 1) Sem leitura confiável não dá para decidir nada sobre o nível
  if (leiturasInvalidas >= MAX_LEITURAS_INVALIDAS) {
    return FALHA_SENSOR;
  }

  // 2) Caixa vazia (com histerese)
  if (nivelPct < NIVEL_VAZIA_PCT)          caixaVaziaAtiva = true;
  else if (nivelPct > NIVEL_SAI_VAZIA_PCT) caixaVaziaAtiva = false;
  if (caixaVaziaAtiva) return CAIXA_VAZIA;

  // 3) Possível vazamento: vazão contínua na madrugada por tempo suficiente
  if (ehMadrugada(hora) && vazaoLpm > VAZAO_MIN_VAZAMENTO) {
    if (inicioVazaoMadrugada == 0) inicioVazaoMadrugada = millis();
    if (millis() - inicioVazaoMadrugada >= TEMPO_CONFIRMA_VAZAMENTO_MS) {
      return POSSIVEL_VAZAMENTO;
    }
  } else {
    inicioVazaoMadrugada = 0;  // a vazão parou: zera a contagem
  }

  // 4) Consumo alto
  if (vazaoLpm > VAZAO_ALTA_LPM) return CONSUMO_ALTO;

  return NORMAL;
}

// ===========================================================================
// RESPOSTAS LOCAIS (LED + BUZZER) - funcionam sem internet
// ===========================================================================
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

// Chamada a cada volta do loop: os padrões de piscar/bipar usam millis()
void atualizarAlertas(Estado e) {
  unsigned long agora = millis();

  switch (e) {
    case NORMAL:
      definirCor(LOW, HIGH, LOW);               // verde
      buzzer(false, 0);
      break;

    case CONSUMO_ALTO:
      definirCor(HIGH, HIGH, LOW);              // amarelo (vermelho + verde)
      buzzer(agora % 4000 < 150, 1000);         // bip de 150 ms a cada 4 s
      break;

    case POSSIVEL_VAZAMENTO: {
      bool fase = (agora / 500) % 2 == 0;       // alterna a cada 500 ms
      definirCor(fase, LOW, LOW);               // vermelho piscando
      buzzer(fase, 1500);                       // buzzer intermitente
      break;
    }

    case CAIXA_VAZIA:
      definirCor(HIGH, LOW, LOW);               // vermelho fixo
      buzzer(true, 800);                        // tom grave contínuo
      break;

    case FALHA_SENSOR: {
      bool fase = (agora / 1000) % 2 == 0;
      definirCor(LOW, LOW, fase);               // azul piscando devagar
      unsigned long t = agora % 5000;           // bip duplo a cada 5 s
      buzzer(t < 100 || (t > 250 && t < 350), 2000);
      break;
    }
  }
}

// ===========================================================================
// COMUNICAÇÃO (não bloqueia: se não houver rede, o resto continua rodando)
// ===========================================================================
void iniciarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);  // conecta em segundo plano
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setSocketTimeout(2);           // não trava o loop esperando o broker
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

  // ID de cliente único para não derrubar outro dispositivo no broker
  String idCliente = "caixa-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(idCliente.c_str())) {
    Serial.println("[REDE] MQTT conectado");
  } else {
    Serial.print("[REDE] Falha no MQTT, código ");
    Serial.println(mqtt.state());
  }
}

// Formato UltraLight 2.0 do IoT Agent: atributo|valor|atributo|valor...
//   n = nível (%)   v = vazão (L/min)   h = hora   s = estado
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

// ===========================================================================
// SERIAL
// ===========================================================================
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

// ===========================================================================
// SETUP E LOOP
// ===========================================================================
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

  // 1) MEDIR e DECIDIR
  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    atualizarLeituras();

    Estado novoEstado = decidirEstado();
    if (novoEstado != estadoAtual) {
      Serial.printf(">>> Mudanca de estado: %s -> %s\n",
                    nomeEstado(estadoAtual), nomeEstado(novoEstado));
      estadoAtual = novoEstado;
      publicar();  // avisa a nuvem na hora, sem esperar o próximo envio
    }
    imprimirStatus();
  }

  // 2) ALERTAR (local, sempre)
  atualizarAlertas(estadoAtual);

  // 3) ENVIAR (quando houver rede)
  manterConexao();
  if (agora - ultimoEnvio >= INTERVALO_ENVIO_MS) {
    ultimoEnvio = agora;
    publicar();
  }
}