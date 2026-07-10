// ================================================================
//  AutoVRP — NODO VALVULA (Heltec WiFi LoRa 32 V3)
//  NEMA17 + TB6600 + HC-SR04 + DHT11 + INA219 + LoRa
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
#define PIN_STEP  47
#define PIN_DIR   48
#define PASOS_VUELTA  2072
#define VUELTAS_TOTAL 17
#define PASOS_TOTAL   (PASOS_VUELTA * VUELTAS_TOTAL)  // 35224

int  velPulso  = 1500;   // microsegundos entre pulsos (1500=lento, 500=rapido)
int  posicion  = 0;
bool motorActivo = false;

// ── DHT11 ────────────────────────────────────────────────────────
#define PIN_DHT 38
DHTesp dht;
float temperatura = 0.0;
float humedad     = 0.0;

// ── INA219 (I2C custom: SDA=41, SCL=42) ──────────────────────────
TwoWire Wire2 = TwoWire(1);
Adafruit_INA219 ina219;
bool inaOK = false;
float corriente_mA  = 0.0;
float voltaje_V     = 0.0;
float potencia_mW   = 0.0;
#define CORRIENTE_MAX_MA 2500.0   // 2.5A → para motor, ajustar segun driver

// ── HC-SR04 + Kill switch ─────────────────────────────────────────
#define TRIG     35
#define ECHO     33
#define PIN_RELE 26
#define DIST_ADVERTENCIA 30
#define DIST_CRITICO     10
#define DIST_APAGAR       6

long distancia   = 0;
int  nivelCodigo = 0;   // 0=seco 1=adv 2=crit 3=peligro

unsigned long ultimoEnvio  = 0;
unsigned long ultimoDHT    = 0;
unsigned long ultimoINA    = 0;
#define INTERVALO_ENVIO 3000
#define INTERVALO_DHT   5000
#define INTERVALO_INA    500

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ─────────────────────────────────────────────────────────────────

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
  if (corriente_mA < 0) corriente_mA = 0;  // filtrar negativo por offset
}

// Mover motor — verifica sobrecorriente y kill switch en cada paso
bool mover(bool horario, int pasos) {
  if (nivelCodigo >= 3) { Serial.println("BLOQUEADO: nivel agua"); return false; }
  digitalWrite(PIN_DIR, horario ? HIGH : LOW);
  delay(5);
  motorActivo = true;
  dibujarOLED();

  for (int i = 0; i < pasos; i++) {
    // Kill switch
    distancia = medirCM();
    if (distancia <= DIST_APAGAR) {
      digitalWrite(PIN_RELE, HIGH);
      nivelCodigo = 3;
      motorActivo = false;
      Serial.println("MOTOR PARADO: kill switch agua");
      return false;
    }

    // Proteccion sobrecorriente INA219 (cada 50 pasos)
    if (inaOK && i % 50 == 0) {
      leerINA();
      if (corriente_mA > CORRIENTE_MAX_MA) {
        motorActivo = false;
        Serial.println("MOTOR PARADO: sobrecorriente " + String(corriente_mA,0) + "mA");
        return false;
      }
    }

    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(velPulso);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(velPulso);
    posicion += horario ? 1 : -1;
  }

  motorActivo = false;
  return true;
}

void enviarEstado() {
  if (!loraOK) return;
  String msg = "OK P:"  + String(posicion)        +
               " PSI:0.0"                          +
               " BOYA:" + String(nivelCodigo)      +
               " DIST:" + String(distancia)        +
               " T:"    + String(temperatura, 1)   +
               " H:"    + String(humedad, 1)       +
               " I:"    + String(corriente_mA, 0)  +
               " V:"    + String(voltaje_V, 2)     +
               " W:"    + String(potencia_mW, 0);
  radio.transmit(msg);
  Serial.println("TX: " + msg);
  radio.startReceive();
}

// Homing: avanza en direccion CERRAR (I) a baja velocidad hasta detectar
// sobrecorriente, luego fija posicion = 0
void hacerHoming() {
  Serial.println("HOMING inicio...");
  motorActivo = true;
  digitalWrite(PIN_DIR, LOW);  // direccion CERRAR (I)
  delay(5);

  int velHoming = 2000;  // lento para no golpear fuerte el tope
  for (int i = 0; i < PASOS_TOTAL + 500; i++) {
    if (inaOK && i % 30 == 0) {
      leerINA();
      if (corriente_mA > CORRIENTE_MAX_MA) {
        posicion = 0;
        motorActivo = false;
        Serial.println("HOMING OK — tope detectado en paso " + String(i) + " | " + String(corriente_mA, 0) + " mA");
        return;
      }
    }
    digitalWrite(PIN_STEP, HIGH); delayMicroseconds(velHoming);
    digitalWrite(PIN_STEP, LOW);  delayMicroseconds(velHoming);
  }
  // Si no encontro tope en PASOS_TOTAL+500 pasos, asume posicion 0 igual
  posicion = 0;
  motorActivo = false;
  Serial.println("HOMING: tope no detectado por INA, posicion forzada a 0");
}

void procesarComando(String cmd) {
  cmd.trim();
  Serial.println("CMD: " + cmd);

  if (cmd == "STOP") {
    motorActivo = false;

  } else if (cmd == "INICIO") {
    hacerHoming();
    enviarEstado();
    return;

  } else if (cmd == "LEER") {
    enviarEstado();
    return;

  } else if (cmd.startsWith("VEL:")) {
    // Cambiar velocidad: VEL:1000  (microsegundos, 300=rapido 3000=lento)
    int v = cmd.substring(4).toInt();
    if (v >= 300 && v <= 5000) velPulso = v;
    Serial.println("Velocidad: " + String(velPulso) + " us");

  } else if (cmd.charAt(0) == 'D') {
    int pasos = cmd.substring(1).toInt();
    if (pasos > 0) mover(true,  pasos);

  } else if (cmd.charAt(0) == 'I') {
    int pasos = cmd.substring(1).toInt();
    if (pasos > 0) mover(false, pasos);
  }

  enviarEstado();
}

void dibujarOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "== NODO AUTOVRP ==");
  oled.drawLine(0, 11, 128, 11);
  oled.drawString(0, 13, "P:" + String(posicion) + " V:" + String(velPulso) + "us");
  oled.drawString(0, 23, "Dist:" + String(distancia) + "cm T:" + String(temperatura,0) + "C H:" + String(humedad,0) + "%");
  oled.drawString(0, 33, "I:" + String(corriente_mA,0) + "mA " + String(voltaje_V,1) + "V");

  oled.setFont(ArialMT_Plain_16);
  if (nivelCodigo == 3)      oled.drawString(0, 44, "!! PELIGRO !!");
  else if (nivelCodigo == 2) oled.drawString(0, 44, "!! CRITICO !!");
  else if (nivelCodigo == 1) oled.drawString(0, 44, "ADVERTENCIA");
  else                       oled.drawString(0, 44, motorActivo ? "MOVIENDO" : "LISTO");

  oled.display();
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

  // Motor
  pinMode(PIN_STEP, OUTPUT); pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW); digitalWrite(PIN_DIR, LOW);

  // Kill switch
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(PIN_RELE, OUTPUT); digitalWrite(PIN_RELE, LOW);

  // DHT11
  dht.setup(PIN_DHT, DHTesp::DHT11);
  Serial.println("DHT11 init OK");

  // INA219 en I2C custom (SDA=41, SCL=42)
  Wire2.begin(41, 42);
  if (ina219.begin(&Wire2)) {
    inaOK = true;
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 OK");
  } else {
    Serial.println("INA219 NO encontrado");
  }

  // LoRa
  int r = radio.begin(915.0, 125.0, 9, 7, 0xAB, 10, 8);
  if (r != RADIOLIB_ERR_NONE) {
    Serial.println("LoRa FALLO: " + String(r));
    loraOK = false;
  } else {
    loraOK = true;
    radio.setDio1Action(isrRX);
    radio.startReceive();
    Serial.println("LoRa OK");
  }

  Serial.println("=== Nodo listo ===");
  dibujarOLED();
}

void loop() {
  unsigned long ahora = millis();

  // Leer DHT11 cada 5s
  if (ahora - ultimoDHT >= INTERVALO_DHT) {
    ultimoDHT = ahora;
    leerDHT();
  }

  // Leer INA219 cada 500ms
  if (ahora - ultimoINA >= INTERVALO_INA) {
    ultimoINA = ahora;
    leerINA();
  }

  // Medir distancia y nivel
  distancia = medirCM();
  if      (distancia <= DIST_APAGAR)      { nivelCodigo = 3; digitalWrite(PIN_RELE, HIGH); }
  else if (distancia <= DIST_CRITICO)     { nivelCodigo = 2; digitalWrite(PIN_RELE, LOW);  }
  else if (distancia <= DIST_ADVERTENCIA) { nivelCodigo = 1; digitalWrite(PIN_RELE, LOW);  }
  else                                    { nivelCodigo = 0; digitalWrite(PIN_RELE, LOW);  }

  // Enviar estado periodico
  if (loraOK && ahora - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = ahora;
    enviarEstado();
  }

  // Recibir comando LoRa
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

  dibujarOLED();
  delay(200);
}
