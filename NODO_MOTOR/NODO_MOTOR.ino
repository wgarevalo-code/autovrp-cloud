// ================================================================
//  AutoVRP — NODO COMPLETO v2.0 (Heltec WiFi LoRa 32 V3)
//  Motor + P1 + P2 + HC-SR04 + DHT11 + INA219 + Botones + LoRa
// ================================================================
#include <RadioLib.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#include <DHTesp.h>
#include <Adafruit_INA219.h>

// ── LoRa ────────────────────────────────────────────────────────
#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
volatile bool banderaRX = false;
bool loraOK = false;

// ── OLED ────────────────────────────────────────────────────────
SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ── Motor ───────────────────────────────────────────────────────
#define PIN_STEP     47
#define PIN_DIR      48
#define PASOS_VUELTA 2072
#define PASOS_TOTAL  35224
#define PASOS_CLICK  50
#define VEL_PULSO    1500
int  posicion    = 0;
bool motorActivo = false;

// ── Transductor P1 (aguas arriba) GPIO1 ─────────────────────────
#define PIN_P1          1
const float ADC_CERRADO_P1 = 627.0;
const float ADC_ABIERTO_P1 = 1914.0;
const float PSI_ABIERTO_P1 = 40.0;
float m_P1 = 0.0;
float b_P1 = 0.0;
float presionP1 = 0.0;

// ── Transductor P2 (aguas abajo) GPIO2 ──────────────────────────
#define PIN_P2          2
const float PENDIENTE_P2  = 76.7013;
const float INTERCEPTO_P2 = -30.22;
float presionP2 = 0.0;

// ── DHT11 ───────────────────────────────────────────────────────
#define PIN_DHT 38
DHTesp dht;
float temperatura = 0.0;
float humedad     = 0.0;

// ── INA219 ──────────────────────────────────────────────────────
TwoWire Wire2 = TwoWire(1);
Adafruit_INA219 ina219;
bool  inaOK     = false;
float corriente = 0.0;
float voltajeINA = 0.0;
#define CORRIENTE_MAX 2500.0

// ── HC-SR04 + Kill switch ────────────────────────────────────────
#define TRIG     35
#define ECHO     33
#define PIN_RELE 26
#define DIST_ADVERTENCIA 30
#define DIST_CRITICO     10
#define DIST_APAGAR       6
long distancia   = 0;
int  nivelCodigo = 0;

// ── Botones ─────────────────────────────────────────────────────
#define BTN_MANUAL 39
#define BTN_DER    40
#define BTN_IZQ    37
bool estadoAntManual = LOW;
bool estadoAntDer    = LOW;
bool estadoAntIzq    = LOW;
bool modoManual      = false;

// ── Tiempos ─────────────────────────────────────────────────────
unsigned long ultimoEnvio  = 0;
unsigned long ultimoDHT    = 0;
unsigned long ultimoINA    = 0;
unsigned long ultimoSensor = 0;
#define INTERVALO_ENVIO  3000
#define INTERVALO_DHT    5000
#define INTERVALO_INA     500
#define INTERVALO_SENSOR  300

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ── Funciones ───────────────────────────────────────────────────

int leerEstable(int pin, int muestras) {
  long suma = 0;
  for (int i = 0; i < muestras; i++) {
    suma += analogRead(pin);
    delay(3);
  }
  return suma / muestras;
}

long medirCM() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long dur = pulseIn(ECHO, HIGH, 30000);
  return dur / 58;
}

void leerTransductores() {
  // P1 aguas arriba
  int rawP1  = leerEstable(PIN_P1, 25);
  presionP1  = (rawP1 * m_P1) + b_P1;
  if (presionP1 < 0)   presionP1 = 0;
  if (presionP1 > 100) presionP1 = 100;

  // P2 aguas abajo
  int   rawP2 = leerEstable(PIN_P2, 25);
  float vP2   = (rawP2 * 3.3) / 4095.0;
  presionP2   = (PENDIENTE_P2 * vP2) + INTERCEPTO_P2;
  if (presionP2 < 0)   presionP2 = 0;
  if (presionP2 > 175) presionP2 = 175;
}

void leerDHT() {
  TempAndHumidity th = dht.getTempAndHumidity();
  if (!isnan(th.temperature)) temperatura = th.temperature;
  if (!isnan(th.humidity))    humedad     = th.humidity;
}

void leerINA() {
  if (!inaOK) return;
  voltajeINA = ina219.getBusVoltage_V();
  corriente  = ina219.getCurrent_mA();
  if (corriente < 0) corriente = 0;
}

void dibujarOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);

  if (modoManual) {
    oled.drawString(0, 0, "=== MODO MANUAL ===");
    oled.drawLine(0, 11, 128, 11);
    oled.drawString(0, 13, "P1:" + String(presionP1, 1) + " PSI");
    oled.drawString(64, 13, "P2:" + String(presionP2, 1) + " PSI");
    oled.drawString(0, 24, "Pos:" + String(posicion) + " pasos");
    oled.drawString(0, 35, "I:" + String(corriente, 0) + "mA T:" + String(temperatura, 0) + "C");
    oled.setFont(ArialMT_Plain_16);
    oled.drawString(0, 46, motorActivo ? "MOVIENDO..." : "LISTO");
  } else {
    oled.drawString(0, 0, "=== NODO AUTOVRP ===");
    oled.drawLine(0, 11, 128, 11);
    oled.drawString(0, 13, "P1:" + String(presionP1, 1) + " P2:" + String(presionP2, 1) + " PSI");
    oled.drawString(0, 24, "Dist:" + String(distancia) + "cm T:" + String(temperatura, 0) + "C");
    oled.drawString(0, 35, "I:" + String(corriente, 0) + "mA Pos:" + String(posicion));
    oled.setFont(ArialMT_Plain_16);
    if      (nivelCodigo == 3) oled.drawString(0, 46, "!! PELIGRO !!");
    else if (nivelCodigo == 2) oled.drawString(0, 46, "!! CRITICO !!");
    else if (nivelCodigo == 1) oled.drawString(0, 46, "ADVERTENCIA");
    else                       oled.drawString(0, 46, "SISTEMA OK");
  }
  oled.display();
}

bool moverPasos(bool horario, int pasos) {
  if (nivelCodigo >= 3) {
    Serial.println("BLOQUEADO: nivel agua");
    return false;
  }
  if (horario  && posicion >= PASOS_TOTAL) {
    Serial.println("LIMITE MAX");
    return false;
  }
  if (!horario && posicion <= 0) {
    Serial.println("LIMITE MIN");
    return false;
  }

  // Validar P2 no supere P1 al cerrar
  if (horario && presionP2 >= presionP1) {
    Serial.println("BLOQUEADO: P2 >= P1");
    oled.clear();
    oled.setFont(ArialMT_Plain_10);
    oled.drawString(0, 10, "BLOQUEADO");
    oled.drawString(0, 25, "P2 no puede >= P1");
    oled.drawString(0, 40, "P1:" + String(presionP1,1) + " P2:" + String(presionP2,1));
    oled.display();
    delay(1500);
    return false;
  }

  digitalWrite(PIN_DIR, horario ? HIGH : LOW);
  delay(5);
  motorActivo = true;

  for (int i = 0; i < pasos; i++) {
    if (nivelCodigo >= 3) { motorActivo = false; return false; }
    if (inaOK && i % 50 == 0) {
      leerINA();
      if (corriente > CORRIENTE_MAX) {
        motorActivo = false;
        Serial.println("STOP sobrecorriente: " + String(corriente, 0) + "mA");
        return false;
      }
    }
    digitalWrite(PIN_STEP, HIGH); delayMicroseconds(VEL_PULSO);
    digitalWrite(PIN_STEP, LOW);  delayMicroseconds(VEL_PULSO);
    posicion += horario ? 1 : -1;
  }

  motorActivo = false;
  return true;
}

void enviarEstado() {
  if (!loraOK) return;
  String msg = "OK P:"   + String(posicion)      +
               " PSI:"   + String(presionP2, 1)  +
               " P1:"    + String(presionP1, 1)  +
               " BOYA:"  + String(nivelCodigo)   +
               " DIST:"  + String(distancia)     +
               " T:"     + String(temperatura,1) +
               " H:"     + String(humedad,1)     +
               " I:"     + String(corriente,0)   +
               " MODO:"  + String(modoManual ? "M":"A");
  radio.transmit(msg);
  Serial.println("TX: " + msg);
  radio.startReceive();
}

void procesarComando(String cmd) {
  cmd.trim();
  Serial.println("CMD: " + cmd);
  if (cmd == "STOP")        { motorActivo = false; }
  else if (cmd == "LEER")   { enviarEstado(); return; }
  else if (cmd == "MODOM")  { modoManual = true;  }
  else if (cmd == "MODOA")  { modoManual = false; }
  else if (cmd.charAt(0) == 'D') {
    int p = cmd.substring(1).toInt();
    if (p > 0) moverPasos(true, p);
  } else if (cmd.charAt(0) == 'I') {
    int p = cmd.substring(1).toInt();
    if (p > 0) moverPasos(false, p);
  }
  enviarEstado();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // OLED
  pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); delay(100);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW); delay(50);
  digitalWrite(RST_OLED, HIGH); delay(50);
  oled.init(); oled.flipScreenVertically(); oled.setBrightness(255);

  // ADC — 11dB para rango completo 0-3.3V
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Calibración P1
  m_P1 = PSI_ABIERTO_P1 / (ADC_ABIERTO_P1 - ADC_CERRADO_P1);
  b_P1 = -1.0 * (m_P1 * ADC_CERRADO_P1);

  // Motor
  pinMode(PIN_STEP, OUTPUT); pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW); digitalWrite(PIN_DIR, LOW);

  // Kill switch
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(PIN_RELE, OUTPUT); digitalWrite(PIN_RELE, LOW);

  // DHT11
  dht.setup(PIN_DHT, DHTesp::DHT11);

  // INA219
  Wire2.begin(41, 42);
  if (ina219.begin(&Wire2)) {
    inaOK = true;
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 OK");
  } else {
    Serial.println("INA219 NO encontrado");
  }

  // Botones con pulldown interno — no flotan
  pinMode(BTN_MANUAL, INPUT_PULLDOWN);
  pinMode(BTN_DER,    INPUT_PULLDOWN);
  pinMode(BTN_IZQ,    INPUT_PULLDOWN);

  // LoRa
  int r = radio.begin(915.0, 125.0, 9, 7, 0xAB, 10, 8);
  if (r != RADIOLIB_ERR_NONE) {
    Serial.println("LoRa FALLO: " + String(r));
  } else {
    loraOK = true;
    radio.setDio1Action(isrRX);
    radio.startReceive();
    Serial.println("LoRa OK");
  }

  Serial.println("=== Nodo AutoVRP v2.0 listo ===");
  dibujarOLED();
}

void loop() {
  unsigned long ahora = millis();

  // ── Botones ──────────────────────────────────────────────────
  bool manualActual = digitalRead(BTN_MANUAL);
  bool derActual    = digitalRead(BTN_DER);
  bool izqActual    = digitalRead(BTN_IZQ);

  if (manualActual == HIGH && estadoAntManual == LOW) {
    modoManual = !modoManual;
    Serial.println(modoManual ? "MODO MANUAL ON" : "MODO AUTO ON");
    enviarEstado();
    dibujarOLED();
  }

  if (derActual == HIGH && estadoAntDer == LOW) {
    if (modoManual) {
      Serial.println(">>> DER");
      moverPasos(true, PASOS_CLICK);
      enviarEstado();
      dibujarOLED();
    } else {
      oled.clear();
      oled.setFont(ArialMT_Plain_16);
      oled.drawString(0, 10, "BLOQUEADO");
      oled.drawString(0, 30, "Activa MANUAL");
      oled.display();
      delay(1000);
      dibujarOLED();
    }
  }

  if (izqActual == HIGH && estadoAntIzq == LOW) {
    if (modoManual) {
      Serial.println("<<< IZQ");
      moverPasos(false, PASOS_CLICK);
      enviarEstado();
      dibujarOLED();
    } else {
      oled.clear();
      oled.setFont(ArialMT_Plain_16);
      oled.drawString(0, 10, "BLOQUEADO");
      oled.drawString(0, 30, "Activa MANUAL");
      oled.display();
      delay(1000);
      dibujarOLED();
    }
  }

  estadoAntManual = manualActual;
  estadoAntDer    = derActual;
  estadoAntIzq    = izqActual;

  // ── Sensores periódicos ───────────────────────────────────────
  if (ahora - ultimoSensor >= INTERVALO_SENSOR) {
    ultimoSensor = ahora;
    leerTransductores();
    distancia = medirCM();

    if      (distancia <= DIST_APAGAR)      { nivelCodigo = 3; digitalWrite(PIN_RELE, HIGH); }
    else if (distancia <= DIST_CRITICO)     { nivelCodigo = 2; digitalWrite(PIN_RELE, LOW);  }
    else if (distancia <= DIST_ADVERTENCIA) { nivelCodigo = 1; digitalWrite(PIN_RELE, LOW);  }
    else                                    { nivelCodigo = 0; digitalWrite(PIN_RELE, LOW);  }

    dibujarOLED();
  }

  if (ahora - ultimoDHT >= INTERVALO_DHT) {
    ultimoDHT = ahora;
    leerDHT();
  }

  if (ahora - ultimoINA >= INTERVALO_INA) {
    ultimoINA = ahora;
    leerINA();
  }

  // ── Enviar estado ─────────────────────────────────────────────
  if (loraOK && ahora - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = ahora;
    enviarEstado();
  }

  // ── Recibir LoRa ─────────────────────────────────────────────
  if (loraOK && banderaRX) {
    banderaRX = false;
    String rxStr;
    if (radio.readData(rxStr) == RADIOLIB_ERR_NONE) {
      rxStr.trim();
      Serial.println("RX: " + rxStr);
      procesarComando(rxStr);
    } else {
      radio.startReceive();
    }
  }
}
