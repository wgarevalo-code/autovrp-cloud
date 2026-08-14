// ================================================================
//  AutoVRP — GATEWAY (Heltec WiFi LoRa 32 V3)
//  + Deteccion nodo desconectado (LEER = ping cada 3s)
//  + Alertas Telegram con cooldowns independientes
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

const char* TELEGRAM_TOKEN   = "8820660886:AAHBrK9C2JZ_liCR4wkKSZUr7YEIy9Aek3s";
const char* TELEGRAM_CHAT_ID = "8150132531";

#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
WebServer server(80);

volatile bool banderaRX = false;
bool wifiConectado       = false;
String estadoActual      = "INICIANDO";

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
bool  nodoManual      = false;

// ── Deteccion nodo desconectado ───────────────────────────────────
unsigned long ultimaRespuestaNodo = 0;
#define TIMEOUT_NODO 12000
bool nodoConectado     = false;
bool alertaNodoEnviada = false;

// ── Alertas Telegram (cooldowns independientes) ───────────────────
bool alertaInundacionEnviada = false;
bool alertaP2AltaEnviada     = false;
unsigned long tgUltimoNodo       = 0;
unsigned long tgUltimoInundacion = 0;
unsigned long tgUltimoGeneral    = 0;
#define TG_MIN_NODO       5000
#define TG_MIN_INUNDACION 8000
#define TG_MIN_GENERAL    10000

// ── PID ──────────────────────────────────────────────────────────
float setpointPSI   = 20.0;
float Kp            = 8.0;
float Ki            = 0.5;
float Kd            = 2.0;
float errorAnterior = 0.0;
float integralError = 0.0;
float integralMax   = 200.0;
bool  modoAuto      = false;

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

unsigned long ultimaLectura   = 0;
#define INTERVALO_LEER 3000

unsigned long ultimoEnvioNube = 0;
#define INTERVALO_NUBE 4000

unsigned long ultimoCheckCmd  = 0;
#define INTERVALO_CMD 2500

bool esperandoRespuesta   = false;
unsigned long tiempoEnvio = 0;
#define TIMEOUT_RESPUESTA 6000

void ARDUINO_ISR_ATTR isrRX() { banderaRX = true; }

// ── Telegram ─────────────────────────────────────────────────────
void enviarTelegramNodo(String mensaje) {
  if (!wifiConectado) return;
  if (millis() - tgUltimoNodo < TG_MIN_NODO) return;
  tgUltimoNodo = millis();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) +
               "&text=" + mensaje;
  url.replace(" ", "%20");
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  Serial.println("TG nodo: " + String(code));
  http.end();
}

void enviarTelegramInundacion(String mensaje) {
  if (!wifiConectado) return;
  if (millis() - tgUltimoInundacion < TG_MIN_INUNDACION) return;
  tgUltimoInundacion = millis();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) +
               "&text=" + mensaje;
  url.replace(" ", "%20");
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  Serial.println("TG inundacion: " + String(code));
  http.end();
}

void enviarTelegramGeneral(String mensaje) {
  if (!wifiConectado) return;
  if (millis() - tgUltimoGeneral < TG_MIN_GENERAL) return;
  tgUltimoGeneral = millis();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) +
               "&text=" + mensaje;
  url.replace(" ", "%20");
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  Serial.println("TG general: " + String(code));
  http.end();
}

void verificarAlertas() {
  bool sinNodo = !nodoConectado && (millis() - ultimaRespuestaNodo > TIMEOUT_NODO);

  if (sinNodo && !alertaNodoEnviada) {
    alertaNodoEnviada = true;
    enviarTelegramNodo("Nodo DESCONECTADO - Sin respuesta LoRa (>12s)");
    Serial.println("ALERTA: nodo desconectado");
  }
  if (nodoConectado && alertaNodoEnviada) {
    alertaNodoEnviada = false;
    enviarTelegramNodo("Nodo reconectado - RSSI:" + String(rssiVal) + "dBm");
  }

  if (nivelInundacion >= 3 && !alertaInundacionEnviada) {
    alertaInundacionEnviada = true;
    enviarTelegramInundacion("PELIGRO INUNDACION - Nivel critico! Dist:" + String(distanciaCM) + "cm");
  }
  if (nivelInundacion == 0) alertaInundacionEnviada = false;

  if (nivelInundacion == 2 && !alertaP2AltaEnviada) {
    alertaP2AltaEnviada = true;
    enviarTelegramInundacion("Nivel CRITICO de agua - Dist:" + String(distanciaCM) + "cm");
  }
  if (nivelInundacion < 2) alertaP2AltaEnviada = false;
}

// ── Helpers ───────────────────────────────────────────────────────
String getCalidad(int rssi) {
  if (rssi == 0)   return "Sin senal";
  if (rssi >= -60) return "Excelente";
  if (rssi >= -75) return "Buena";
  if (rssi >= -90) return "Regular";
  return "Debil";
}

int getBarras(int rssi) {
  if (rssi == 0)   return 0;
  if (rssi >= -60) return 5;
  if (rssi >= -70) return 4;
  if (rssi >= -80) return 3;
  if (rssi >= -90) return 2;
  return 1;
}

void agregarHistorial(float val) {
  historialPresion[historialIdx] = val;
  historialIdx = (historialIdx + 1) % HISTORIAL_SIZE;
  if (historialIdx == 0) historialLleno = true;
}

String getHistorialJSON() {
  String j = "[";
  int total = historialLleno ? HISTORIAL_SIZE : historialIdx;
  for (int i = 0; i < total; i++) {
    if (i > 0) j += ",";
    j += String(historialPresion[i], 1);
  }
  return j + "]";
}

void enviarLoRa(String msg) {
  radio.transmit(msg);
  esperandoRespuesta = true;
  tiempoEnvio = millis();
  Serial.println("TX: " + msg);
  radio.startReceive();
}

void controlPID() {
  if (millis() - ultimoMovimientoGW < TIEMPO_ESTABILIZACION) return;
  float error = setpointPSI - presionPSI_P2;
  if (abs(error) <= ZONA_MUERTA_PSI) { estadoActual = "ESTABLE"; return; }
  integralError += error;
  integralError  = constrain(integralError, -integralMax, integralMax);
  float derivada = error - errorAnterior;
  float salida   = Kp*error + Ki*integralError + Kd*derivada;
  errorAnterior  = error;
  int pasos = constrain((int)abs(salida), 10, 200);
  if (error > 0) { enviarLoRa("I" + String(pasos)); estadoActual = "PID:APRETAR"; }
  else           { enviarLoRa("D" + String(pasos)); estadoActual = "PID:AFLOJAR"; }
  ultimoMovimientoGW = millis();
  dibujarOLED();
}

void dibujarOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== GATEWAY AUTOVRP ===");
  oled.drawLine(0, 11, 128, 11);
  if (!nodoConectado) {
    oled.drawString(0, 13, wifiConectado ? "WiFi OK" : "Sin WiFi");
    oled.setFont(ArialMT_Plain_16);
    oled.drawString(0, 28, "SIN NODO");
    oled.setFont(ArialMT_Plain_10);
    oled.drawString(0, 50, "Esperando nodo LoRa...");
  } else {
    oled.drawString(0, 13, "P1:" + String(presionPSI_P1,1) + " P2:" + String(presionPSI_P2,1) + " PSI");
    oled.drawString(0, 24, "SP:" + String(setpointPSI,0) + " T:" + String(temperatura,0) + "C H:" + String(humedad,0) + "%");
    String nivelStr = "";
    if      (nivelInundacion == 1) nivelStr = " ADVT";
    else if (nivelInundacion == 2) nivelStr = " CRIT";
    else if (nivelInundacion >= 3) nivelStr = " PELIGRO";
    oled.drawString(0, 35, (modoAuto ? "[AUTO] " : "[MAN] ") + estadoActual + nivelStr);
    oled.drawRect(0, 47, 55, 8);
    if (rssiVal != 0) {
      int bw = map(constrain(rssiVal, -110, -30), -110, -30, 0, 53);
      oled.fillRect(1, 48, bw, 6);
    }
    oled.drawString(57, 47, getCalidad(rssiVal));
  }
  oled.display();
}

void parsearRespuesta(String rxStr) {
  int idxPos = rxStr.indexOf("P:");
  if (idxPos >= 0) {
    int idxE = rxStr.indexOf(' ', idxPos);
    if (idxE < 0) idxE = rxStr.length();
    posicionNodo = rxStr.substring(idxPos+2, idxE).toInt();
  }
  int idxPsi1 = rxStr.indexOf(" P1:");
  if (idxPsi1 >= 0) {
    int idxE = rxStr.indexOf(' ', idxPsi1+1);
    if (idxE < 0) idxE = rxStr.length();
    presionPSI_P1 = rxStr.substring(idxPsi1+4, idxE).toFloat();
  }
  int idxPsi2 = rxStr.indexOf(" PSI:");
  if (idxPsi2 >= 0) {
    int idxE = rxStr.indexOf(' ', idxPsi2+1);
    if (idxE < 0) idxE = rxStr.length();
    presionPSI_P2 = rxStr.substring(idxPsi2+5, idxE).toFloat();
    agregarHistorial(presionPSI_P2);
  }
  int idxMod = rxStr.indexOf("MODO:");
  if (idxMod >= 0) {
    int idxE = rxStr.indexOf(' ', idxMod);
    if (idxE < 0) idxE = rxStr.length();
    nodoManual = (rxStr.substring(idxMod+5, idxE) == "M");
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
  int idxLuz = rxStr.indexOf(" LUZ:");
  if (idxLuz >= 0) {
    int idxE = rxStr.indexOf(' ', idxLuz+1);
    if (idxE < 0) idxE = rxStr.length();
    luzEncendida = (rxStr.substring(idxLuz+5, idxE) == "1");
  }
}

void checkCmdNube() {
  if (!wifiConectado) return;
  HTTPClient http;
  http.begin(URL_CMD_POLL);
  http.setTimeout(500);
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
    luzEncendida = !luzEncendida;
    enviarLoRa(luzEncendida ? "LUZON" : "LUZOFF"); return;
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
  enviarLoRa(cmd);
  if (cmd.charAt(0)=='D' || cmd.charAt(0)=='I') ultimoMovimientoGW = millis();
}

void enviarANube() {
  if (!wifiConectado) return;
  float grados = ((float)posicionNodo / 400.0) * 360.0;
  String json = "{";
  json += "\"presionP1\":"       + String(presionPSI_P1, 2)                     + ",";
  json += "\"presionP2\":"       + String(presionPSI_P2, 2)                     + ",";
  json += "\"humedad\":"         + String(humedad, 1)                           + ",";
  json += "\"temperatura\":"     + String(temperatura, 1)                       + ",";
  json += "\"nivelInundacion\":" + String(nivelInundacion)                      + ",";
  json += "\"distanciaCM\":"     + String(distanciaCM)                          + ",";
  json += "\"boyaMojada\":"      + String(nivelInundacion > 0 ? "true":"false") + ",";
  json += "\"luzEncendida\":"    + String(luzEncendida ? "true":"false")        + ",";
  json += "\"movimiento\":"      + String(movimientoDet ? "true":"false")       + ",";
  json += "\"pasos\":"           + String(posicionNodo)                         + ",";
  json += "\"grados\":"          + String(grados, 1)                            + ",";
  json += "\"rssi\":"            + String(rssiVal)                              + ",";
  json += "\"snr\":"             + String(snrVal, 1)                            + ",";
  json += "\"calidad\":\""       + getCalidad(rssiVal)                          + "\",";
  json += "\"barras\":"          + String(getBarras(rssiVal))                   + ",";
  json += "\"modoAuto\":"        + String(modoAuto ? "true":"false")            + ",";
  json += "\"setpoint\":"        + String(setpointPSI, 1)                       + ",";
  json += "\"corrienteMA\":"     + String(corrienteMA, 0)                       + ",";
  json += "\"voltajeV\":"        + String(voltajeV, 2)                          + ",";
  json += "\"potenciaMW\":"      + String(potenciaMW, 0)                        + ",";
  json += "\"nodoManual\":"      + String(nodoManual ? "true":"false")          + ",";
  json += "\"nodoConectado\":"   + String(nodoConectado ? "true":"false")       + ",";
  json += "\"estado\":\""        + estadoActual                                 + "\"";
  json += "}";

  HTTPClient http;
  http.begin(URL_NUBE);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  if (code == 200) {
    String resp = http.getString();
    int idxCmd = resp.indexOf("\"cmd\":\"");
    if (idxCmd >= 0) {
      int ini = idxCmd + 7;
      int fin = resp.indexOf("\"", ini);
      if (fin > ini) ejecutarCmdNube(resp.substring(ini, fin));
    }
  }
  http.end();
  Serial.println("Nube: " + String(code));
}

void handleRoot() {
  server.sendHeader("Location", "https://autovrp-cloud-production.up.railway.app", true);
  server.send(302, "text/plain", "");
}

void handleDatos() {
  float grados = ((float)posicionNodo / 400.0) * 360.0;
  String json = "{";
  json += "\"rssi\":"            + String(rssiVal)                      + ",";
  json += "\"snr\":"             + String(snrVal,1)                     + ",";
  json += "\"pasos\":"           + String(posicionNodo)                 + ",";
  json += "\"grados\":"          + String(grados,1)                     + ",";
  json += "\"presionP1\":"       + String(presionPSI_P1,2)              + ",";
  json += "\"presionP2\":"       + String(presionPSI_P2,2)              + ",";
  json += "\"humedad\":"         + String(humedad,1)                    + ",";
  json += "\"temperatura\":"     + String(temperatura,1)                + ",";
  json += "\"boyaMojada\":"      + String(boyaMojada?"true":"false")    + ",";
  json += "\"nivelInundacion\":" + String(nivelInundacion)              + ",";
  json += "\"distanciaCM\":"     + String(distanciaCM)                  + ",";
  json += "\"luzEncendida\":"    + String(luzEncendida?"true":"false")  + ",";
  json += "\"movimiento\":"      + String(movimientoDet?"true":"false") + ",";
  json += "\"calidad\":\""       + getCalidad(rssiVal)                  + "\",";
  json += "\"barras\":"          + String(getBarras(rssiVal))           + ",";
  json += "\"modoAuto\":"        + String(modoAuto?"true":"false")      + ",";
  json += "\"setpoint\":"        + String(setpointPSI,1)                + ",";
  json += "\"nodoConectado\":"   + String(nodoConectado?"true":"false") + ",";
  json += "\"estado\":\""        + estadoActual                         + "\",";
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
    tgUltimoGeneral = millis();
    enviarTelegramGeneral("AutoVRP Gateway en linea - Esperando nodo LoRa");
  } else {
    estadoActual = "SIN WIFI";
    Serial.println("WiFi FALLO");
  }
  dibujarOLED();

  int r = radio.begin(915.0, 125.0, 9, 7, 0xAB, 10, 8);
  if (r != RADIOLIB_ERR_NONE) { estadoActual = "ERR LORA"; dibujarOLED(); while(true); }
  radio.setDio1Action(isrRX);
  radio.startReceive();

  // Arranca el temporizador desde el boot para detectar nodo ausente desde inicio
  ultimaRespuestaNodo = millis();

  estadoActual = "ESCUCHANDO"; dibujarOLED();
  Serial.println("Gateway listo");
}

void loop() {
  if (wifiConectado) server.handleClient();

  // ── Deteccion nodo caido (estaba conectado y dejo de responder) ──
  if (nodoConectado && millis() - ultimaRespuestaNodo > TIMEOUT_NODO) {
    nodoConectado = false;
    estadoActual  = "SIN NODO";
    rssiVal       = 0;
    snrVal        = 0;
    dibujarOLED();
    Serial.println("*** NODO DESCONECTADO ***");
  }

  // ── Timeout respuesta LoRa ────────────────────────────────────
  if (esperandoRespuesta && millis()-tiempoEnvio > TIMEOUT_RESPUESTA) {
    esperandoRespuesta = false;
    Serial.println("Timeout LoRa");
  }

  // ── Poll comandos nube ────────────────────────────────────────
  if (!esperandoRespuesta && !banderaRX && millis()-ultimoCheckCmd >= INTERVALO_CMD) {
    ultimoCheckCmd = millis();
    checkCmdNube();
  }

  // ── Enviar a nube ─────────────────────────────────────────────
  if (!esperandoRespuesta && !banderaRX && millis()-ultimoEnvioNube >= INTERVALO_NUBE) {
    ultimoEnvioNube = millis();
    enviarANube();
  }

  // ── LEER = ping al nodo cada 3s ──────────────────────────────
  if (!esperandoRespuesta && millis()-ultimaLectura >= INTERVALO_LEER) {
    ultimaLectura = millis();
    enviarLoRa("LEER");
  }

  // ── PID automatico ────────────────────────────────────────────
  if (modoAuto && !esperandoRespuesta && millis()-ultimoPID >= INTERVALO_PID) {
    ultimoPID = millis();
    controlPID();
  }

  // ── Recibir LoRa ─────────────────────────────────────────────
  if (banderaRX) {
    banderaRX = false;
    esperandoRespuesta = false;
    String rxStr;
    int r = radio.readData(rxStr);
    if (r == RADIOLIB_ERR_NONE) {
      rxStr.trim();
      rssiVal = (int)radio.getRSSI();
      snrVal  = radio.getSNR();
      bool primerConexion = !nodoConectado;
      nodoConectado = true;
      ultimaRespuestaNodo = millis();
      estadoActual = "CONECTADO";
      if (primerConexion) Serial.println("Nodo conectado RSSI:" + String(rssiVal));
      Serial.println("RX: " + rxStr + " RSSI:" + String(rssiVal));
      if (rxStr.startsWith("OK")) parsearRespuesta(rxStr);
    }
    radio.startReceive();
    dibujarOLED();
  }

  // ── Verificar alertas ─────────────────────────────────────────
  verificarAlertas();
}
