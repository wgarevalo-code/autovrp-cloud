// ================================================================
//  AutoVRP — GATEWAY (Heltec WiFi LoRa 32 V3)
//  MODIFICADO: envia datos a servidor en la nube (Railway)
// ================================================================
#include <RadioLib.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#include <WiFiManager.h>
#include <WebServer.h>
#include <WiFi.h>
#include <HTTPClient.h>

const char* URL_NUBE     = "https://autovrp-cloud-production.up.railway.app/actualizar";
const char* URL_CMD_POLL = "https://autovrp-cloud-production.up.railway.app/cmd-pendiente";

#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
WebServer server(80);

volatile bool banderaRX  = false;
bool wifiConectado        = false;
String estadoActual       = "INICIANDO";

int   rssiVal      = 0;
float snrVal       = 0.0;
int   posicionNodo = 0;

float presionPSI_P2   = 0.0;
float presionPSI_P1   = 30.0;
float humedad         = 0.0;
float temperatura     = 0.0;
bool  boyaMojada      = false;
bool  luzEncendida    = false;
bool  movimientoDet   = false;
int   nivelInundacion = 0;
long  distanciaCM     = 0;
float corrienteMA     = 0.0;
float voltajeV        = 0.0;
float potenciaMW      = 0.0;
bool  nodoManual      = false;   // true cuando tecnico activo modo manual en el nodo

// ── PID ──────────────────────────────────────────────────────────
float setpointPSI     = 20.0;
float Kp              = 8.0;
float Ki              = 0.5;
float Kd              = 2.0;
float errorAnterior   = 0.0;
float integralError   = 0.0;
float integralMax     = 200.0;
bool  modoAuto        = false;

#define ZONA_MUERTA_PSI       1.5
#define TIEMPO_ESTABILIZACION 6000
#define INTERVALO_PID         4000

unsigned long ultimoPID          = 0;
unsigned long ultimoMovimientoGW = 0;

// ── Historial ────────────────────────────────────────────────────
#define HISTORIAL_SIZE 30
float historialPresion[HISTORIAL_SIZE];
int   historialIdx   = 0;
bool  historialLleno = false;

unsigned long ultimaLectura = 0;
#define INTERVALO_LEER 3000

// ── Envio a nube ─────────────────────────────────────────────────
unsigned long ultimoEnvioNube = 0;
#define INTERVALO_NUBE 3500   // envia cada 3.5s (mayor que INTERVALO_LEER)

// ── Poll rapido de comandos ───────────────────────────────────────
unsigned long ultimoCheckCmd = 0;
#define INTERVALO_CMD 500     // consulta Railway por comandos cada 500ms

bool esperandoRespuesta = false;
unsigned long tiempoEnvio = 0;
#define TIMEOUT_RESPUESTA 6000

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ── Enviar datos al servidor Railway ─────────────────────────────
void checkCmdNube() {
  if (!wifiConectado) return;
  HTTPClient http;
  http.begin(URL_CMD_POLL);
  http.setTimeout(800);
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    int idxCmd = resp.indexOf("\"cmd\":\"");
    if (idxCmd >= 0) {
      int ini = idxCmd + 7;
      int fin = resp.indexOf("\"", ini);
      if (fin > ini) {
        String cmd = resp.substring(ini, fin);
        if (cmd.length() > 0) {
          Serial.println("CMD rapido: " + cmd);
          ejecutarCmdNube(cmd);
        }
      }
    }
  }
  http.end();
}

void ejecutarCmdNube(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  Serial.println("CMD nube: " + cmd);

  if (cmd == "STOP") {
    modoAuto = false; enviarLoRa("STOP"); estadoActual = "PARADA"; dibujarOLED(); return;
  }
  if (cmd == "LUZTOGGLE") {
    luzEncendida = !luzEncendida; enviarLoRa(luzEncendida ? "LUZON" : "LUZOFF"); return;
  }
  if (cmd.startsWith("SET")) {
    float sp = cmd.substring(3).toFloat();
    if (sp < presionPSI_P1) { setpointPSI = sp; integralError = 0; errorAnterior = 0; } return;
  }
  if (cmd.startsWith("PID")) {
    String v = cmd.substring(3); int c1 = v.indexOf(','), c2 = v.lastIndexOf(',');
    if (c1>0 && c2>c1) { Kp=v.substring(0,c1).toFloat(); Ki=v.substring(c1+1,c2).toFloat(); Kd=v.substring(c2+1).toFloat(); } return;
  }
  if (cmd.startsWith("MODO")) {
    modoAuto = (cmd.substring(4) == "1"); integralError = 0; errorAnterior = 0; ultimoMovimientoGW = 0;
    estadoActual = modoAuto ? "AUTO PID" : "MANUAL"; dibujarOLED(); return;
  }
  // Todo lo demas (D, I, STOP, VEL:, INICIO, etc.) va directo al nodo
  enviarLoRa(cmd);
  if (cmd.charAt(0)=='D' || cmd.charAt(0)=='I') ultimoMovimientoGW = millis();
}

void enviarANube() {
  if (!wifiConectado) { Serial.println("Nube: SIN WIFI"); return; }

  float grados = ((float)posicionNodo / 400.0) * 360.0;

  String json = "{";
  json += "\"presionP1\":"       + String(presionPSI_P1, 2)                   + ",";
  json += "\"presionP2\":"       + String(presionPSI_P2, 2)                   + ",";
  json += "\"humedad\":"         + String(humedad, 1)                         + ",";
  json += "\"temperatura\":"     + String(temperatura, 1)                     + ",";
  json += "\"nivelInundacion\":" + String(nivelInundacion)                    + ",";
  json += "\"distanciaCM\":"     + String(distanciaCM)                        + ",";
  json += "\"boyaMojada\":"      + String(nivelInundacion > 0 ? "true":"false") + ",";
  json += "\"luzEncendida\":"    + String(luzEncendida ? "true":"false")      + ",";
  json += "\"movimiento\":"      + String(movimientoDet ? "true":"false")     + ",";
  json += "\"pasos\":"           + String(posicionNodo)                       + ",";
  json += "\"grados\":"          + String(grados, 1)                          + ",";
  json += "\"rssi\":"            + String(rssiVal)                            + ",";
  json += "\"snr\":"             + String(snrVal, 1)                          + ",";
  json += "\"calidad\":\""       + getCalidad(rssiVal)                        + "\",";
  json += "\"barras\":"          + String(getBarras(rssiVal))                 + ",";
  json += "\"modoAuto\":"        + String(modoAuto ? "true":"false")          + ",";
  json += "\"setpoint\":"        + String(setpointPSI, 1)                     + ",";
  json += "\"corrienteMA\":"     + String(corrienteMA, 0)                    + ",";
  json += "\"voltajeV\":"        + String(voltajeV, 2)                       + ",";
  json += "\"potenciaMW\":"      + String(potenciaMW, 0)                     + ",";
  json += "\"nodoManual\":"      + String(nodoManual ? "true" : "false")     + ",";
  json += "\"estado\":\""        + estadoActual                               + "\"";
  json += "}";

  HTTPClient http;
  http.begin(URL_NUBE);
  http.setTimeout(1500);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);

  // Leer respuesta para ver si hay comando pendiente del dashboard
  if (code == 200) {
    String resp = http.getString();
    int idxCmd = resp.indexOf("\"cmd\":\"");
    if (idxCmd >= 0) {
      int ini = idxCmd + 7;
      int fin = resp.indexOf("\"", ini);
      if (fin > ini) {
        String cmd = resp.substring(ini, fin);
        ejecutarCmdNube(cmd);
      }
    }
  }

  http.end();
  Serial.println("Nube: " + String(code));
}

// ─────────────────────────────────────────────────────────────────

void dibujarOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== GATEWAY AUTOVRP ===");
  oled.drawLine(0, 11, 128, 11);
  oled.drawString(0, 13, wifiConectado ? "autovrp-cloud.up.railway.app" : "Sin WiFi");
  oled.drawString(0, 24, "P2:" + String(presionPSI_P2, 1) + " SP:" + String(setpointPSI, 0) + " PSI");
  String nivelStr = "";
  if      (nivelInundacion == 1) nivelStr = " ADVT";
  else if (nivelInundacion == 2) nivelStr = " CRIT";
  else if (nivelInundacion >= 3) nivelStr = " PELIGRO";
  oled.drawString(0, 35, (modoAuto ? "[AUTO] " : "[MAN] ") + estadoActual + nivelStr);
  oled.drawRect(0, 47, 55, 8);
  if (rssiVal != 0) {
    int bw = map(constrain(rssiVal, -110, -30), -110, -30, 0, 53);
    oled.fillRect(1, 48, bw, 6);
    oled.drawString(58, 47, String(rssiVal) + "dBm");
  } else {
    oled.drawString(58, 47, "Sin nodo");
  }
  oled.drawString(0, 56, "Pos:" + String(posicionNodo) + " pasos");
  oled.display();
}

void agregarHistorial(float v) {
  historialPresion[historialIdx] = v;
  historialIdx = (historialIdx + 1) % HISTORIAL_SIZE;
  if (historialIdx == 0) historialLleno = true;
}

String getHistorialJSON() {
  String json = "[";
  int total = historialLleno ? HISTORIAL_SIZE : historialIdx;
  int start = historialLleno ? historialIdx : 0;
  for (int i = 0; i < total; i++) {
    int idx = (start + i) % HISTORIAL_SIZE;
    json += String(historialPresion[idx], 1);
    if (i < total - 1) json += ",";
  }
  return json + "]";
}

String getCalidad(int rssi) {
  if (rssi == 0)  return "Sin senal";
  if (rssi > -55) return "Excelente";
  if (rssi > -70) return "Buena";
  if (rssi > -85) return "Regular";
  return "Debil";
}

int getBarras(int rssi) {
  if (rssi == 0)  return 0;
  if (rssi > -50) return 5;
  if (rssi > -65) return 4;
  if (rssi > -75) return 3;
  if (rssi > -85) return 2;
  return 1;
}

void enviarLoRa(String cmd) {
  radio.transmit(cmd);
  esperandoRespuesta = true;
  tiempoEnvio = millis();
  banderaRX = false;
  radio.startReceive();
  Serial.println("TX: " + cmd);
}

void controlPID() {
  if (!modoAuto) return;
  if (nodoManual) { estadoActual = "MANUAL FISICO"; dibujarOLED(); return; }
  if (millis() - ultimoMovimientoGW < TIEMPO_ESTABILIZACION) {
    estadoActual = "ESTABILIZANDO"; dibujarOLED(); return;
  }
  float error = setpointPSI - presionPSI_P2;
  float dt    = INTERVALO_PID / 1000.0;
  if (abs(error) < ZONA_MUERTA_PSI) {
    estadoActual = "EN SETPOINT"; integralError = 0; dibujarOLED(); return;
  }
  integralError += error * dt;
  integralError  = constrain(integralError, -integralMax, integralMax);
  float derivada = (error - errorAnterior) / dt;
  float salida   = (Kp * error) + (Ki * integralError) + (Kd * derivada);
  errorAnterior  = error;
  int pasos = constrain((int)abs(salida), 1, 150);
  String cmd = (salida > 0) ? "D" + String(pasos) : "I" + String(pasos);
  estadoActual = cmd;
  ultimoMovimientoGW = millis();
  enviarLoRa(cmd);
  dibujarOLED();
}

void parsearRespuesta(String rxStr) {
  int idxP = rxStr.indexOf("P:");
  if (idxP >= 0) {
    int idxE = rxStr.indexOf(' ', idxP);
    if (idxE < 0) idxE = rxStr.length();
    posicionNodo = rxStr.substring(idxP+2, idxE).toInt();
  }
  // PSI1 = aguas arriba (P1), PSI2 = aguas abajo (P2)
  int idxPsi1 = rxStr.indexOf("PSI1:");
  if (idxPsi1 >= 0) {
    int idxE = rxStr.indexOf(' ', idxPsi1);
    if (idxE < 0) idxE = rxStr.length();
    presionPSI_P1 = rxStr.substring(idxPsi1+5, idxE).toFloat();
  }
  int idxPsi2 = rxStr.indexOf("PSI2:");
  if (idxPsi2 >= 0) {
    int idxE = rxStr.indexOf(' ', idxPsi2);
    if (idxE < 0) idxE = rxStr.length();
    presionPSI_P2 = rxStr.substring(idxPsi2+5, idxE).toFloat();
    agregarHistorial(presionPSI_P2);
  }
  // MOD:M = manual fisico activo, MOD:A = auto
  int idxMod = rxStr.indexOf("MOD:");
  if (idxMod >= 0) {
    int idxE = rxStr.indexOf(' ', idxMod);
    if (idxE < 0) idxE = rxStr.length();
    String mod = rxStr.substring(idxMod+4, idxE);
    nodoManual = (mod == "M");
  }
  int idxBoya = rxStr.indexOf("BOYA:");
  if (idxBoya >= 0) {
    int idxE = rxStr.indexOf(' ', idxBoya);
    if (idxE < 0) idxE = rxStr.length();
    nivelInundacion = rxStr.substring(idxBoya+5, idxE).toInt();
    boyaMojada = (nivelInundacion > 0);
  }
  int idxDist = rxStr.indexOf("DIST:");
  if (idxDist >= 0) {
    int idxE = rxStr.indexOf(' ', idxDist);
    if (idxE < 0) idxE = rxStr.length();
    distanciaCM = rxStr.substring(idxDist+5, idxE).toInt();
  }
  int idxT = rxStr.indexOf(" T:");
  if (idxT >= 0) {
    int idxE = rxStr.indexOf(' ', idxT+1);
    if (idxE < 0) idxE = rxStr.length();
    temperatura = rxStr.substring(idxT+3, idxE).toFloat();
  }
  int idxH = rxStr.indexOf(" H:");
  if (idxH >= 0) {
    int idxE = rxStr.indexOf(' ', idxH+1);
    if (idxE < 0) idxE = rxStr.length();
    humedad = rxStr.substring(idxH+3, idxE).toFloat();
  }
  int idxI = rxStr.indexOf(" I:");
  if (idxI >= 0) {
    int idxE = rxStr.indexOf(' ', idxI+1);
    if (idxE < 0) idxE = rxStr.length();
    corrienteMA = rxStr.substring(idxI+3, idxE).toFloat();
  }
  int idxV = rxStr.indexOf(" V:");
  if (idxV >= 0) {
    int idxE = rxStr.indexOf(' ', idxV+1);
    if (idxE < 0) idxE = rxStr.length();
    voltajeV = rxStr.substring(idxV+3, idxE).toFloat();
  }
  int idxW = rxStr.indexOf(" W:");
  if (idxW >= 0) {
    int idxE = rxStr.indexOf(' ', idxW+1);
    if (idxE < 0) idxE = rxStr.length();
    potenciaMW = rxStr.substring(idxW+3, idxE).toFloat();
  }
}

// ── Web server local ──────────────────────────────────────────────
void handleRoot() {
  server.sendHeader("Location", "https://autovrp-cloud-production.up.railway.app", true);
  server.send(302, "text/plain", "");
}

void handleDatos() {
  float grados = ((float)posicionNodo / 400.0) * 360.0;
  String json = "{";
  json += "\"rssi\":"            + String(rssiVal)                            + ",";
  json += "\"snr\":"             + String(snrVal,1)                           + ",";
  json += "\"pasos\":"           + String(posicionNodo)                       + ",";
  json += "\"grados\":"          + String(grados,1)                           + ",";
  json += "\"presionP1\":"       + String(presionPSI_P1,2)                    + ",";
  json += "\"presionP2\":"       + String(presionPSI_P2,2)                    + ",";
  json += "\"humedad\":"         + String(humedad,1)                          + ",";
  json += "\"temperatura\":"     + String(temperatura,1)                      + ",";
  json += "\"boyaMojada\":"      + String(boyaMojada?"true":"false")          + ",";
  json += "\"nivelInundacion\":" + String(nivelInundacion)                    + ",";
  json += "\"distanciaCM\":"     + String(distanciaCM)                        + ",";
  json += "\"luzEncendida\":"    + String(luzEncendida?"true":"false")        + ",";
  json += "\"movimiento\":"      + String(movimientoDet?"true":"false")       + ",";
  json += "\"calidad\":\""       + getCalidad(rssiVal)                        + "\",";
  json += "\"barras\":"          + String(getBarras(rssiVal))                 + ",";
  json += "\"modoAuto\":"        + String(modoAuto?"true":"false")            + ",";
  json += "\"setpoint\":"        + String(setpointPSI,1)                      + ",";
  json += "\"corrienteMA\":"     + String(corrienteMA, 0)                    + ",";
  json += "\"voltajeV\":"        + String(voltajeV, 2)                       + ",";
  json += "\"potenciaMW\":"      + String(potenciaMW, 0)                     + ",";
  json += "\"estado\":\""        + estadoActual                               + "\",";
  json += "\"historial\":"       + getHistorialJSON();
  json += "}";
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!server.hasArg("c")) { server.send(400,"text/plain","Sin cmd"); return; }
  String cmd = server.arg("c");
  cmd.trim();
  if (cmd == "STOP") {
    modoAuto = false; enviarLoRa("STOP"); estadoActual = "PARADA";
    server.send(200,"text/plain","OK"); dibujarOLED(); return;
  }
  if (cmd == "LUZTOGGLE") {
    luzEncendida = !luzEncendida;
    enviarLoRa(luzEncendida ? "LUZON" : "LUZOFF");
    server.send(200,"text/plain","OK"); return;
  }
  if (cmd.startsWith("SET")) {
    float sp = cmd.substring(3).toFloat();
    if (sp >= presionPSI_P1) { server.send(400,"text/plain","Setpoint supera P1"); return; }
    setpointPSI = sp; integralError = 0; errorAnterior = 0;
    server.send(200,"text/plain","OK"); return;
  }
  if (cmd.startsWith("PID")) {
    String vals = cmd.substring(3);
    int c1 = vals.indexOf(','), c2 = vals.lastIndexOf(',');
    if (c1>0 && c2>c1) {
      Kp = vals.substring(0,c1).toFloat();
      Ki = vals.substring(c1+1,c2).toFloat();
      Kd = vals.substring(c2+1).toFloat();
    }
    server.send(200,"text/plain","OK"); return;
  }
  if (cmd.startsWith("MODO")) {
    modoAuto = (cmd.substring(4) == "1");
    integralError = 0; errorAnterior = 0; ultimoMovimientoGW = 0;
    estadoActual = modoAuto ? "AUTO PID" : "MANUAL";
    server.send(200,"text/plain","OK"); dibujarOLED(); return;
  }
  enviarLoRa(cmd);
  if (cmd.charAt(0)=='D' || cmd.charAt(0)=='I') ultimoMovimientoGW = millis();
  server.send(200,"text/plain","OK");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW); delay(100);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW); delay(50);
  digitalWrite(RST_OLED, HIGH); delay(50);
  oled.init(); oled.flipScreenVertically(); oled.setBrightness(255);
  estadoActual = "Init WiFi..."; dibujarOLED();
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  if (wm.autoConnect("AutoVRP-Setup","autovrp123")) {
    wifiConectado = true;
    server.on("/",      handleRoot);
    server.on("/datos", handleDatos);
    server.on("/cmd",   handleCmd);
    server.begin();
    estadoActual = "WiFi OK";
    Serial.println("WiFi conectado");
    Serial.println("URL: " + String(URL_NUBE));
  } else {
    estadoActual = "SIN WIFI";
    Serial.println("WiFi FALLO");
  }
  dibujarOLED();
  int r = radio.begin(915.0, 125.0, 9, 7, 0xAB, 10, 8);
  if (r != RADIOLIB_ERR_NONE) { estadoActual = "ERR LORA"; dibujarOLED(); while(true); }
  radio.setDio1Action(isrRX);
  radio.startReceive();
  estadoActual = "ESCUCHANDO"; dibujarOLED();
  Serial.println("Gateway listo");
}

void loop() {
  if (wifiConectado) server.handleClient();

  // Timeout respuesta LoRa
  if (esperandoRespuesta && millis()-tiempoEnvio > TIMEOUT_RESPUESTA) {
    esperandoRespuesta = false;
    Serial.println("Timeout LoRa");
  }

  // Poll rapido de comandos cada 500ms (sin bloquear si hay LoRa pendiente)
  if (!esperandoRespuesta && !banderaRX && millis()-ultimoCheckCmd >= INTERVALO_CMD) {
    ultimoCheckCmd = millis();
    checkCmdNube();
  }

  // Enviar a nube (primero, antes de LEER)
  if (!esperandoRespuesta && !banderaRX && millis()-ultimoEnvioNube >= INTERVALO_NUBE) {
    ultimoEnvioNube = millis();
    enviarANube();
  }

  // Enviar LEER al nodo
  if (!esperandoRespuesta && millis()-ultimaLectura >= INTERVALO_LEER) {
    ultimaLectura = millis();
    enviarLoRa("LEER");
  }

  // PID automatico
  if (modoAuto && !esperandoRespuesta && millis()-ultimoPID >= INTERVALO_PID) {
    ultimoPID = millis();
    controlPID();
  }

  // Recibir respuesta LoRa
  if (banderaRX) {
    banderaRX = false;
    esperandoRespuesta = false;
    String rxStr;
    int r = radio.readData(rxStr);
    if (r == RADIOLIB_ERR_NONE) {
      rxStr.trim();
      rssiVal = (int)radio.getRSSI();
      snrVal  = radio.getSNR();
      estadoActual = "CONECTADO";
      Serial.println("RX: " + rxStr + " RSSI:" + String(rssiVal));
      if (rxStr.startsWith("OK")) parsearRespuesta(rxStr);
    }
    radio.startReceive();
    dibujarOLED();
  }
}
