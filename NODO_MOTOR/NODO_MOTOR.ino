// ================================================================
//  AutoVRP — NODO VALVULA (Heltec WiFi LoRa 32 V3)
//  Motor NEMA17 + TB6600 + HC-SR04 + Kill switch + LoRa
// ================================================================
#include <RadioLib.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"

SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ── LoRa ─────────────────────────────────────────────────────────
#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
volatile bool banderaRX = false;
bool loraOK = false;

// ── Motor NEMA17 + TB6600 ─────────────────────────────────────────
#define PIN_STEP  47
#define PIN_DIR   48
#define VEL_PULSO 1500   // microsegundos entre pulsos

// ── Kill switch HC-SR04 ───────────────────────────────────────────
#define TRIG     35
#define ECHO     33
#define PIN_RELE 26

#define DIST_ADVERTENCIA 30
#define DIST_CRITICO     10
#define DIST_APAGAR       6

int  posicion    = 0;
bool motorActivo = false;
long distancia   = 0;
int  nivelCodigo = 0;   // 0=seco 1=advertencia 2=critico 3=peligro

unsigned long ultimoEnvio = 0;
#define INTERVALO_ENVIO 3000

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ─────────────────────────────────────────────────────────────────
long medirCM() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duracion = pulseIn(ECHO, HIGH, 30000);
  return duracion / 58;
}

void mover(bool horario, int pasos) {
  if (nivelCodigo >= 3) return;   // no mover si hay peligro
  digitalWrite(PIN_DIR, horario ? HIGH : LOW);
  delay(5);
  motorActivo = true;
  dibujarOLED();
  for (int i = 0; i < pasos; i++) {
    // Verificar kill switch durante movimiento
    distancia = medirCM();
    if (distancia <= DIST_APAGAR) {
      digitalWrite(PIN_RELE, HIGH);
      nivelCodigo = 3;
      break;
    }
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(VEL_PULSO);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(VEL_PULSO);
    posicion += horario ? 1 : -1;
  }
  motorActivo = false;
}

void enviarEstado() {
  if (!loraOK) return;
  String msg = "OK P:" + String(posicion) +
               " PSI:0.0" +
               " BOYA:" + String(nivelCodigo) +
               " DIST:" + String(distancia);
  radio.transmit(msg);
  Serial.println("TX: " + msg);
  radio.startReceive();
}

void procesarComando(String cmd) {
  cmd.trim();
  Serial.println("CMD: " + cmd);

  if (cmd == "STOP") {
    motorActivo = false;
    Serial.println("Motor parado");

  } else if (cmd == "LEER") {
    // Solo responde con estado actual
    enviarEstado();
    return;   // enviarEstado ya llama radio.startReceive()

  } else if (cmd.charAt(0) == 'D') {
    int pasos = cmd.substring(1).toInt();
    if (pasos > 0 && nivelCodigo < 3) {
      Serial.println("Motor derecha " + String(pasos) + " pasos");
      mover(true, pasos);
      Serial.println("Pos: " + String(posicion));
    }

  } else if (cmd.charAt(0) == 'I') {
    int pasos = cmd.substring(1).toInt();
    if (pasos > 0 && nivelCodigo < 3) {
      Serial.println("Motor izquierda " + String(pasos) + " pasos");
      mover(false, pasos);
      Serial.println("Pos: " + String(posicion));
    }
  }

  // Responder con estado actualizado
  enviarEstado();
}

void dibujarOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== NODO AUTOVRP ===");
  oled.drawLine(0, 11, 128, 11);
  oled.drawString(0, 13, "Pos: " + String(posicion) + " | " + String((float)posicion/2072.0, 1) + " vueltas");
  oled.drawString(0, 24, "Dist: " + String(distancia) + " cm");

  oled.setFont(ArialMT_Plain_16);
  if (nivelCodigo == 3) {
    oled.drawString(0, 35, "!! APAGANDO !!");
  } else if (nivelCodigo == 2) {
    oled.drawString(0, 35, "!! CRITICO !!");
  } else if (nivelCodigo == 1) {
    oled.drawString(0, 35, "ADVERTENCIA");
  } else {
    oled.drawString(0, 35, motorActivo ? "MOVIENDO..." : "LISTO");
  }

  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 56, loraOK ? "LoRa: OK" : "LoRa: ERROR");
  oled.display();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // OLED
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(100);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW); delay(50);
  digitalWrite(RST_OLED, HIGH); delay(50);
  oled.init();
  oled.flipScreenVertically();
  oled.setBrightness(255);

  // Motor
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_DIR, LOW);

  // Kill switch
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, LOW);  // sistema ON al arrancar

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
  // Medir distancia y actualizar nivel
  distancia = medirCM();

  if (distancia <= DIST_APAGAR) {
    nivelCodigo = 3;
    digitalWrite(PIN_RELE, HIGH);   // corta TB6600
  } else if (distancia <= DIST_CRITICO) {
    nivelCodigo = 2;
    digitalWrite(PIN_RELE, LOW);
  } else if (distancia <= DIST_ADVERTENCIA) {
    nivelCodigo = 1;
    digitalWrite(PIN_RELE, LOW);
  } else {
    nivelCodigo = 0;
    digitalWrite(PIN_RELE, LOW);
  }

  // Enviar estado periodicamente (sin esperar comando)
  if (loraOK && millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    enviarEstado();
  }

  // Recibir comando del gateway
  if (loraOK && banderaRX) {
    banderaRX = false;
    String rxStr;
    int r = radio.readData(rxStr);
    if (r == RADIOLIB_ERR_NONE) {
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
