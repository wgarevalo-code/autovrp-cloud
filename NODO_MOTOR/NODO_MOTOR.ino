// ================================================================
//  AutoVRP — NODO VALVULA (Heltec WiFi LoRa 32 V3 / ESP32-S3)
//  NEMA17 + TB6600 + HC-SR04 + DHT11 + INA219 + Transductores P1/P2
//  Botones fisicos: MANUAL(39) DER(40) IZQ(37)
// ================================================================
#include <RadioLib.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#include <DHTesp.h>
#include <Adafruit_INA219.h>

// ── LoRa ─────────────────────────────────────────────────────────
#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
volatile bool banderaRX = false;
bool loraOK = false;

// ── OLED ─────────────────────────────────────────────────────────
SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ── Motor NEMA17 + TB6600 ─────────────────────────────────────────
#define PIN_STEP      47
#define PIN_DIR       48
#define PASOS_VUELTA  2072
#define VUELTAS_TOTAL 17
#define PASOS_BOTON   15      // pasos por toque de boton (ajuste milimetrico)
int  velPulso    = 1500;
int  posicion    = 0;
bool motorActivo = false;

// ── Transductores de presion (voltaje 0.5-4.5V, alim 5V) ─────────
// Divisor de voltaje R1=1k R2=2.7k en señal: escala 4.5V→3.28V
// PIN_P1 y PIN_P2 solo aceptan hasta 3.3V — NO conectar directo al sensor
#define PIN_P1  1    // Aguas arriba  (despues de divisor)
#define PIN_P2  2    // Aguas abajo   (despues de divisor)
#define PSI_MAX      100.0
#define ADC_0PSI     453    // 0.5V * (2.7/3.7) / 3.3V * 4095 = 453
#define ADC_100PSI   4076   // 4.5V * (2.7/3.7) / 3.3V * 4095 = 4076
#define NUM_MUESTRAS 10     // promedio para estabilizar lectura

float presionP1 = 0.0;
float presionP2 = 0.0;

// ── Botones fisicos ───────────────────────────────────────────────
#define BTN_MANUAL  39
#define BTN_DER     40
#define BTN_IZQ     37
#define DEBOUNCE    300
unsigned long tsManual = 0, tsDer = 0, tsIzq = 0;

// ── Modo manual/auto ──────────────────────────────────────────────
bool modoManual = false;

// ── DHT11 ────────────────────────────────────────────────────────
#define PIN_DHT 38
DHTesp dht;
float temperatura = 0.0;
float humedad     = 0.0;

// ── INA219 (Wire1: SDA=41, SCL=42) ───────────────────────────────
TwoWire Wire2 = TwoWire(1);
Adafruit_INA219 ina219;
bool  inaOK       = false;
float corriente_mA = 0.0;
float voltaje_V    = 0.0;
float potencia_mW  = 0.0;
#define CORRIENTE_MAX_MA 2500.0

// ── HC-SR04 + Kill switch ─────────────────────────────────────────
#define TRIG     35
#define ECHO     33
#define PIN_RELE 26
#define DIST_ADVERTENCIA 30
#define DIST_CRITICO     10
#define DIST_APAGAR       6

long distancia   = 0;
int  nivelCodigo = 0;

// ── Timers ────────────────────────────────────────────────────────
unsigned long ultimoEnvio   = 0;
unsigned long ultimoDHT     = 0;
unsigned long ultimoINA     = 0;
unsigned long ultimoPresion = 0;
#define INTERVALO_ENVIO   3000
#define INTERVALO_DHT     5000
#define INTERVALO_INA      500
#define INTERVALO_PRESION  200

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ─────────────────────────────────────────────────────────────────

float leerPresionPSI(int pin) {
  long suma = 0;
  for (int i = 0; i < NUM_MUESTRAS; i++) {
    suma += analogRead(pin);
    delayMicroseconds(500);
  }
  float adc = suma / NUM_MUESTRAS;
  float psi = (adc - ADC_0PSI) / (float)(ADC_100PSI - ADC_0PSI) * PSI_MAX;
  return constrain(psi, 0.0, PSI_MAX);
}

long medirCM() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long dur = pulseIn(ECHO, HIGH, 30000);
  return dur / 58;
}

void leerDHT() {
  TempAndHumidity th = dht.getTempAndHumidity();
  if (!isnan(th.temperature)) temperatura = th.temperature;
  if (!isnan(th.humidity))    humedad     = th.humidity;
}

void leerINA() {
  if (!inaOK) return;
  voltaje_V    = ina219.getBusVoltage_V();
  corriente_mA = ina219.getCurrent_mA();
  potencia_mW  = ina219.getPower_mW();
  if (corriente_mA < 0) corriente_mA = 0;
}

// Mover motor — con proteccion sobrecorriente y kill switch
bool mover(bool horario, int pasos) {
  if (nivelCodigo >= 3) return false;
  digitalWrite(PIN_DIR, horario ? HIGH : LOW);
  delay(5);
  motorActivo = true;
  for (int i = 0; i < pasos; i++) {
    distancia = medirCM();
    if (distancia <= DIST_APAGAR) {
      digitalWrite(PIN_RELE, HIGH); nivelCodigo = 3;
      motorActivo = false; return false;
    }
    if (inaOK && i % 50 == 0) {
      leerINA();
      if (corriente_mA > CORRIENTE_MAX_MA) { motorActivo = false; return false; }
    }
    digitalWrite(PIN_STEP, HIGH); delayMicroseconds(velPulso);
    digitalWrite(PIN_STEP, LOW);  delayMicroseconds(velPulso);
    posicion += horario ? 1 : -1;
  }
  motorActivo = false;
  return true;
}

// Homing: busca tope por sobrecorriente
void hacerHoming() {
  Serial.println("HOMING...");
  motorActivo = true;
  digitalWrite(PIN_DIR, LOW); delay(5);
  for (int i = 0; i < (PASOS_VUELTA * VUELTAS_TOTAL + 500); i++) {
    if (inaOK && i % 30 == 0) {
      leerINA();
      if (corriente_mA > CORRIENTE_MAX_MA) { posicion = 0; motorActivo = false; return; }
    }
    digitalWrite(PIN_STEP, HIGH); delayMicroseconds(2000);
    digitalWrite(PIN_STEP, LOW);  delayMicroseconds(2000);
  }
  posicion = 0; motorActivo = false;
}

void enviarEstado() {
  if (!loraOK) return;
  String msg = "OK P:"   + String(posicion)       +
               " PSI1:"  + String(presionP1, 1)   +
               " PSI2:"  + String(presionP2, 1)   +
               " BOYA:"  + String(nivelCodigo)    +
               " DIST:"  + String(distancia)      +
               " T:"     + String(temperatura, 1) +
               " H:"     + String(humedad, 1)     +
               " I:"     + String(corriente_mA, 0)+
               " V:"     + String(voltaje_V, 2)   +
               " W:"     + String(potencia_mW, 0) +
               " MOD:"   + (modoManual ? "M" : "A");
  radio.transmit(msg);
  Serial.println("TX: " + msg);
  radio.startReceive();
}

// ── OLED ─────────────────────────────────────────────────────────

void oledAuto() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== NODO AUTOVRP ===");
  oled.drawLine(0, 11, 128, 11);
  oled.drawString(0, 13, "P:" + String(posicion) + " T:" + String(temperatura,0) + "C H:" + String(humedad,0) + "%");
  oled.drawString(0, 23, "Dist:" + String(distancia) + "cm I:" + String(corriente_mA,0) + "mA");
  // Presiones en grande
  oled.setFont(ArialMT_Plain_16);
  oled.drawString(0, 34, "P1:" + String(presionP1,1));
  oled.drawString(0, 50, "P2:" + String(presionP2,1) + " PSI");
  oled.display();
}

void oledManual() {
  oled.clear();
  // Encabezado modo manual
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, ">>> MODO MANUAL <<<");
  oled.drawLine(0, 11, 128, 11);
  // Presiones grandes
  oled.setFont(ArialMT_Plain_16);
  oled.drawString(0, 14, "P1: " + String(presionP1, 1) + " PSI");
  oled.drawString(0, 32, "P2: " + String(presionP2, 1) + " PSI");
  // Barra progreso P2 vs P1
  oled.setFont(ArialMT_Plain_10);
  int barW = (presionP1 > 0) ? (int)((presionP2 / presionP1) * 110.0) : 0;
  barW = constrain(barW, 0, 110);
  oled.drawRect(0, 52, 112, 8);
  oled.fillRect(1, 53, barW, 6);
  oled.drawString(114, 52, String((int)((presionP2/max(presionP1,0.1f))*100)) + "%");
  oled.display();
}

// ── COMANDOS ─────────────────────────────────────────────────────

void procesarComando(String cmd) {
  cmd.trim();
  Serial.println("CMD: " + cmd);

  if (cmd == "STOP")   { motorActivo = false; }
  else if (cmd == "LEER") { enviarEstado(); return; }
  else if (cmd == "INICIO") { hacerHoming(); }
  else if (cmd == "MANUAL_ON")  { modoManual = true;  Serial.println("MODO MANUAL"); }
  else if (cmd == "MANUAL_OFF") { modoManual = false; Serial.println("MODO AUTO"); }
  else if (cmd == "DER") { if (modoManual) mover(true,  PASOS_BOTON); }
  else if (cmd == "IZQ") { if (modoManual) mover(false, PASOS_BOTON); }
  else if (cmd.startsWith("VEL:")) {
    int v = cmd.substring(4).toInt();
    if (v >= 300 && v <= 5000) velPulso = v;
  }
  else if (cmd.charAt(0) == 'D' && !modoManual) {
    int p = cmd.substring(1).toInt(); if (p > 0) mover(true,  p);
  }
  else if (cmd.charAt(0) == 'I' && !modoManual) {
    int p = cmd.substring(1).toInt(); if (p > 0) mover(false, p);
  }

  enviarEstado();
}

// ─────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(300);

  // OLED
  pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); delay(100);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW); delay(50);
  digitalWrite(RST_OLED, HIGH); delay(50);
  oled.init(); oled.flipScreenVertically(); oled.setBrightness(255);

  // Motor
  pinMode(PIN_STEP, OUTPUT); pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW); digitalWrite(PIN_DIR, LOW);

  // Botones
  pinMode(BTN_MANUAL, INPUT);
  pinMode(BTN_DER,    INPUT);
  pinMode(BTN_IZQ,    INPUT);

  // Kill switch / HC-SR04
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(PIN_RELE, OUTPUT); digitalWrite(PIN_RELE, LOW);

  // ADC presion
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);   // rango 0-3.3V
  pinMode(PIN_P1, INPUT);
  pinMode(PIN_P2, INPUT);

  // DHT11
  dht.setup(PIN_DHT, DHTesp::DHT11);

  // INA219
  Wire2.begin(41, 42);
  if (ina219.begin(&Wire2)) {
    inaOK = true;
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 OK");
  }

  // LoRa
  int r = radio.begin(915.0, 125.0, 9, 7, 0xAB, 10, 8);
  if (r == RADIOLIB_ERR_NONE) {
    loraOK = true;
    radio.setDio1Action(isrRX);
    radio.startReceive();
    Serial.println("LoRa OK");
  }

  Serial.println("=== Nodo listo ===");
  oledAuto();
}

void loop() {
  unsigned long ahora = millis();

  // ── Botones fisicos ──────────────────────────────────────────
  if (digitalRead(BTN_MANUAL) == HIGH && ahora - tsManual > DEBOUNCE) {
    tsManual = ahora;
    modoManual = !modoManual;
    Serial.println(modoManual ? "BTN MANUAL ON" : "BTN MANUAL OFF");
    enviarEstado();   // avisa al gateway del cambio de modo
  }

  if (modoManual) {
    if (digitalRead(BTN_DER) == HIGH && ahora - tsDer > DEBOUNCE) {
      tsDer = ahora;
      mover(true, PASOS_BOTON);
      enviarEstado();
    }
    if (digitalRead(BTN_IZQ) == HIGH && ahora - tsIzq > DEBOUNCE) {
      tsIzq = ahora;
      mover(false, PASOS_BOTON);
      enviarEstado();
    }
  }

  // ── Sensores periodicos ──────────────────────────────────────
  if (ahora - ultimoPresion >= INTERVALO_PRESION) {
    ultimoPresion = ahora;
    presionP1 = leerPresionPSI(PIN_P1);
    presionP2 = leerPresionPSI(PIN_P2);
  }
  if (ahora - ultimoDHT >= INTERVALO_DHT) { ultimoDHT = ahora; leerDHT(); }
  if (ahora - ultimoINA >= INTERVALO_INA)  { ultimoINA = ahora; leerINA(); }

  // ── Kill switch agua ─────────────────────────────────────────
  distancia = medirCM();
  if      (distancia <= DIST_APAGAR)      { nivelCodigo = 3; digitalWrite(PIN_RELE, HIGH); }
  else if (distancia <= DIST_CRITICO)     { nivelCodigo = 2; digitalWrite(PIN_RELE, LOW); }
  else if (distancia <= DIST_ADVERTENCIA) { nivelCodigo = 1; digitalWrite(PIN_RELE, LOW); }
  else                                    { nivelCodigo = 0; digitalWrite(PIN_RELE, LOW); }

  // ── Envio LoRa periodico ─────────────────────────────────────
  if (loraOK && ahora - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = ahora;
    enviarEstado();
  }

  // ── Recibir comandos ─────────────────────────────────────────
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

  // ── OLED según modo ──────────────────────────────────────────
  if (modoManual) oledManual();
  else            oledAuto();

  delay(100);
}
