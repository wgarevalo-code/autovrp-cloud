// ================================================================
//  AutoVRP — Servidor en la nube
//  Railway / Render — Node.js
// ================================================================
const express = require('express');
const app = express();
app.use(express.json());

const PORT = process.env.PORT || 8080;

// ── Datos en memoria ─────────────────────────────────────────────
let camara1 = {
  presionP1:    30.0,
  presionP2:    0.0,
  humedad:      0.0,
  temperatura:  0.0,
  boyaMojada:   false,
  luzEncendida: false,
  movimiento:   false,
  pasos:        0,
  grados:       0.0,
  rssi:         0,
  snr:          0.0,
  calidad:      'Sin senal',
  barras:       0,
  modoAuto:     false,
  setpoint:     20.0,
  estado:       'ESPERANDO',
  historial:    [],
  ultimaActualizacion: null
};

// ── Recibir datos del Gateway ESP32 ──────────────────────────────
app.post('/actualizar', (req, res) => {
  const d = req.body;
  if (!d) return res.status(400).json({ error: 'Sin datos' });

  camara1 = { ...camara1, ...d, ultimaActualizacion: new Date().toISOString() };

  // Guardar historial (max 30 puntos)
  if (typeof d.presionP2 === 'number') {
    camara1.historial.push(d.presionP2);
    if (camara1.historial.length > 30) camara1.historial.shift();
  }

  res.json({ ok: true });
});

// ── API para el dashboard ─────────────────────────────────────────
app.get('/datos', (req, res) => {
  res.json({ ...camara1, historial: camara1.historial });
});

// ── Endpoints para IFTTT / Google Assistant ───────────────────────
app.get('/camara1/presion', (req, res) => {
  const p2 = camara1.presionP2.toFixed(1);
  const p1 = camara1.presionP1.toFixed(1);
  res.send(`La presion aguas abajo es ${p2} PSI y aguas arriba es ${p1} PSI`);
});

app.get('/camara1/humedad', (req, res) => {
  const h = camara1.humedad.toFixed(0);
  res.send(`La humedad de la camara es ${h} por ciento`);
});

app.get('/camara1/temperatura', (req, res) => {
  const t = camara1.temperatura.toFixed(1);
  res.send(`La temperatura de la camara es ${t} grados centigrados`);
});

app.get('/camara1/estado', (req, res) => {
  const p2   = camara1.presionP2.toFixed(1);
  const sp   = camara1.setpoint.toFixed(0);
  const modo = camara1.modoAuto ? 'automatico' : 'manual';
  const boya = camara1.boyaMojada ? 'ALERTA inundacion detectada' : 'sin inundacion';
  res.send(
    `Camara 1: presion ${p2} PSI, setpoint ${sp} PSI, modo ${modo}, ${boya}`
  );
});

app.get('/camara1/boya', (req, res) => {
  res.send(camara1.boyaMojada
    ? 'ALERTA: el sensor de boya esta mojado, posible inundacion en la camara'
    : 'El sensor de boya esta seco, sin inundacion');
});

// ── Dashboard HTML ────────────────────────────────────────────────
app.get('/', (req, res) => {
  res.sendFile(__dirname + '/public/index.html');
});

app.listen(PORT, () => {
  console.log(`AutoVRP servidor corriendo en puerto ${PORT}`);
});
