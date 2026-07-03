// ================================================================
//  AutoVRP — Servidor en la nube
//  Railway — Node.js + Bot Telegram
// ================================================================
const express = require('express');
const https   = require('https');
const app     = express();
app.use(express.json());

const PORT         = process.env.PORT || 8080;
const TG_TOKEN     = process.env.TG_TOKEN || '8820660886:AAHBrK9C2JZ_liCR4wkKSZUr7YEIy9Aek3s';
const TG_API       = `https://api.telegram.org/bot${TG_TOKEN}`;

// ── Sistema de roles ─────────────────────────────────────────────
// ADMIN: puede controlar + recibe alertas + puede autorizar otros
// VIEWER: solo puede consultar datos + recibe alertas si fue autorizado
// PENDING: escribio al bot pero aun no fue autorizado

const ADMIN_ID = null; // Se asigna automaticamente al primer /miid
let adminId    = null;

const usuarios = new Map();
// usuarios Map: chatId -> { nombre, rol: 'admin'|'viewer'|'pending', alertas: bool }

function getRol(chatId) {
  return usuarios.has(chatId) ? usuarios.get(chatId).rol : 'pending';
}

function esAdmin(chatId)  { return getRol(chatId) === 'admin';  }
function esViewer(chatId) { return ['admin','viewer'].includes(getRol(chatId)); }

function chatsConAlertas() {
  const ids = [];
  usuarios.forEach((u, id) => { if (u.alertas) ids.push(id); });
  return ids;
}

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

// Estado anterior para detectar alertas
let estadoAnterior = { boyaMojada: false, movimiento: false };

// ── Telegram: enviar mensaje ──────────────────────────────────────
function tgEnviar(chatId, texto) {
  const body = JSON.stringify({ chat_id: chatId, text: texto, parse_mode: 'HTML' });
  const url  = new URL(`${TG_API}/sendMessage`);
  const req  = https.request({
    hostname: 'api.telegram.org',
    path: `/bot${TG_TOKEN}/sendMessage`,
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) }
  });
  req.write(body);
  req.end();
}

function tgAlerta(texto) {
  chatsConAlertas().forEach(id => tgEnviar(id, texto));
}

// ── Telegram: procesar comando ────────────────────────────────────
function procesarComando(chatId, texto, req) {
  chatsAutorizados.add(chatId);
  const cmd = texto.trim().toLowerCase().split(' ')[0];

  const d = camara1;
  const sync = d.ultimaActualizacion
    ? new Date(d.ultimaActualizacion).toLocaleTimeString('es-EC')
    : 'Sin datos';

  const rol = getRol(chatId);

  // Registrar usuario nuevo como pending y notificar al admin
  const esNuevo = !usuarios.has(chatId);
  if (esNuevo) {
    const nombre = req?.body?.message?.from?.first_name || 'Usuario';
    usuarios.set(chatId, { nombre, rol: 'pending', alertas: false });
    // Notificar al admin que alguien nuevo entro
    if (adminId && chatId !== adminId) {
      tgEnviar(adminId,
        `👤 <b>Nuevo usuario en el bot:</b>\n` +
        `Nombre: <b>${nombre}</b>\n` +
        `Chat ID: <code>${chatId}</code>\n\n` +
        `Para autorizarlo: /autorizar ${chatId}\n` +
        `Para darle alertas: /alertas ${chatId}`
      );
    }
  }

  // Bloquear comandos a usuarios pending (excepto /start, /ayuda, /miid)
  if (getRol(chatId) === 'pending' && !['/start','/ayuda','/miid'].includes(cmd)) {
    const nombre = usuarios.get(chatId)?.nombre || 'Usuario';
    tgEnviar(chatId,
      `⛔ <b>Acceso pendiente de autorizacion.</b>\n\n` +
      `Hola <b>${nombre}</b>, tu solicitud fue enviada al administrador del sistema.\n\n` +
      `Tu Chat ID es: <code>${chatId}</code>\n\n` +
      `Cuando el administrador te autorice recibiras una notificacion.`
    );
    return;
  }

  switch (cmd) {
    case '/start':
    case '/ayuda': {
      const esAuth = esViewer(chatId);
      tgEnviar(chatId,
        `<b>🔧 AutoVRP — Bot de la Camara 1</b>\n\n` +
        `<b>Consultas:</b>\n` +
        `/presion — Presion P1 y P2\n` +
        `/humedad — Humedad de la camara\n` +
        `/temperatura — Temperatura\n` +
        `/valvula — Posicion de la valvula\n` +
        `/lora — Estado del enlace LoRa\n` +
        `/estado — Resumen completo\n\n` +
        (esAuth ?
        `<b>Control:</b>\n` +
        `/stop — Parada de emergencia\n` +
        `/auto — Modo automatico PID\n` +
        `/manual — Modo manual\n` +
        `/setpoint 20 — Cambiar setpoint\n\n` : '') +
        (esAdmin(chatId) ?
        `<b>Admin:</b>\n` +
        `/usuarios — Ver usuarios registrados\n` +
        `/autorizar [ID] — Dar acceso a un usuario\n` +
        `/revocar [ID] — Quitar acceso\n` +
        `/alertas [ID] — Activar alertas para un usuario\n\n` : '') +
        `<b>Dashboard:</b>\n` +
        `https://autovrp-cloud-production.up.railway.app\n\n` +
        `Tu rol: <b>${rol.toUpperCase()}</b>`
      );
      break;
    }

    case '/miid':
      // Si no hay admin aun, el primero que escriba /miid se convierte en admin
      if (!adminId) {
        adminId = chatId;
        usuarios.set(chatId, { ...usuarios.get(chatId), rol: 'admin', alertas: true });
        tgEnviar(chatId, `✅ <b>Eres el administrador del sistema.</b>\nTu Chat ID: <code>${chatId}</code>\n\nYa puedes usar todos los comandos de control y administrar usuarios.`);
      } else {
        tgEnviar(chatId, `Tu Chat ID es: <code>${chatId}</code>\nRol actual: <b>${rol.toUpperCase()}</b>`);
      }
      break;

    case '/presion':
      tgEnviar(chatId,
        `<b>💧 Presion — Camara 1</b>\n\n` +
        `P1 aguas arriba:  <b>${d.presionP1.toFixed(1)} PSI</b>  (${(d.presionP1/145.038).toFixed(3)} MPa)\n` +
        `P2 aguas abajo:   <b>${d.presionP2.toFixed(1)} PSI</b>  (${(d.presionP2/145.038).toFixed(3)} MPa)\n` +
        `Setpoint:         <b>${d.setpoint.toFixed(1)} PSI</b>\n\n` +
        `🕐 ${sync}`
      );
      break;

    case '/humedad':
      tgEnviar(chatId,
        `<b>💧 Humedad — Camara 1</b>\n\n` +
        `Humedad: <b>${d.humedad.toFixed(0)}%</b>\n` +
        `Estado:  ${d.humedad > 85 ? '⚠️ ALTA' : '✅ Normal'}\n\n` +
        `🕐 ${sync}`
      );
      break;

    case '/temperatura':
      tgEnviar(chatId,
        `<b>🌡️ Temperatura — Camara 1</b>\n\n` +
        `Temperatura: <b>${d.temperatura.toFixed(1)} °C</b>\n` +
        `Estado:  ${d.temperatura > 35 ? '⚠️ ALTA' : '✅ Normal'}\n\n` +
        `🕐 ${sync}`
      );
      break;

    case '/valvula':
      tgEnviar(chatId,
        `<b>🔩 Valvula — Camara 1</b>\n\n` +
        `Posicion: <b>${Math.round((Math.abs(d.pasos)/400)*100)}%</b>\n` +
        `Pasos:    <b>${d.pasos}</b>\n` +
        `Grados:   <b>${d.grados.toFixed(1)}°</b>\n` +
        `Modo:     <b>${d.modoAuto ? 'AUTO PID' : 'MANUAL'}</b>\n\n` +
        `🕐 ${sync}`
      );
      break;

    case '/lora':
      tgEnviar(chatId,
        `<b>📡 Enlace LoRa — Camara 1</b>\n\n` +
        `Estado:   <b>${d.rssi !== 0 ? '✅ CONECTADO' : '❌ SIN NODO'}</b>\n` +
        `RSSI:     <b>${d.rssi} dBm</b>\n` +
        `SNR:      <b>${d.snr} dB</b>\n` +
        `Calidad:  <b>${d.calidad}</b>\n` +
        `Frecuencia: 915 MHz / SF9\n\n` +
        `🕐 ${sync}`
      );
      break;

    case '/estado':
      tgEnviar(chatId,
        `<b>📊 Estado completo — Camara 1</b>\n\n` +
        `<b>Presiones:</b>\n` +
        `  P1: ${d.presionP1.toFixed(1)} PSI | P2: ${d.presionP2.toFixed(1)} PSI\n` +
        `  Setpoint: ${d.setpoint.toFixed(1)} PSI\n\n` +
        `<b>Ambiente:</b>\n` +
        `  Humedad: ${d.humedad.toFixed(0)}%\n` +
        `  Temperatura: ${d.temperatura.toFixed(1)} °C\n\n` +
        `<b>Seguridad:</b>\n` +
        `  Boya: ${d.boyaMojada ? '🚨 MOJADA' : '✅ Seca'}\n` +
        `  Movimiento: ${d.movimiento ? '⚠️ Detectado' : '✅ Sin movimiento'}\n` +
        `  Luz: ${d.luzEncendida ? '💡 Encendida' : '⚫ Apagada'}\n\n` +
        `<b>Control:</b>\n` +
        `  Modo: ${d.modoAuto ? 'AUTO PID' : 'MANUAL'}\n` +
        `  Estado PID: ${d.estado}\n\n` +
        `<b>LoRa:</b> ${d.rssi !== 0 ? '✅ ' + d.calidad : '❌ Sin nodo'} | RSSI: ${d.rssi} dBm\n\n` +
        `🕐 Sincronizacion: ${sync}`
      );
      break;

    case '/usuarios':
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ Solo el administrador puede ver los usuarios.'); break; }
      if (usuarios.size === 0) { tgEnviar(chatId, 'No hay usuarios registrados.'); break; }
      let lista = '<b>👥 Usuarios registrados:</b>\n\n';
      usuarios.forEach((u, id) => {
        lista += `• <b>${u.nombre}</b> (${u.rol.toUpperCase()})\n  ID: <code>${id}</code>  Alertas: ${u.alertas ? '✅' : '❌'}\n\n`;
      });
      tgEnviar(chatId, lista);
      break;

    case '/autorizar': {
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ Solo el administrador puede autorizar usuarios.'); break; }
      const idAuth = parseInt(texto.trim().split(' ')[1]);
      if (!idAuth) { tgEnviar(chatId, 'Uso: /autorizar [ChatID]\nEjemplo: /autorizar 123456789'); break; }
      const uAuth = usuarios.get(idAuth) || { nombre: 'Desconocido', rol: 'pending', alertas: false };
      usuarios.set(idAuth, { ...uAuth, rol: 'viewer', alertas: true });
      tgEnviar(chatId, `✅ Usuario <code>${idAuth}</code> autorizado como VIEWER con alertas.`);
      tgEnviar(idAuth, `✅ <b>Acceso autorizado al bot AutoVRP.</b>\nYa puedes consultar datos y recibir alertas.\nEscribe /ayuda para ver los comandos.`);
      break;
    }

    case '/revocar': {
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ Solo el administrador puede revocar accesos.'); break; }
      const idRev = parseInt(texto.trim().split(' ')[1]);
      if (!idRev) { tgEnviar(chatId, 'Uso: /revocar [ChatID]'); break; }
      if (usuarios.has(idRev)) usuarios.set(idRev, { ...usuarios.get(idRev), rol: 'pending', alertas: false });
      tgEnviar(chatId, `✅ Acceso revocado para <code>${idRev}</code>.`);
      tgEnviar(idRev, `⛔ Tu acceso al bot AutoVRP ha sido revocado.`);
      break;
    }

    case '/alertas': {
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ Solo el administrador puede gestionar alertas.'); break; }
      const idAl = parseInt(texto.trim().split(' ')[1]);
      if (!idAl) { tgEnviar(chatId, 'Uso: /alertas [ChatID]'); break; }
      if (usuarios.has(idAl)) {
        const u = usuarios.get(idAl);
        usuarios.set(idAl, { ...u, alertas: !u.alertas });
        tgEnviar(chatId, `Alertas para <code>${idAl}</code>: ${!u.alertas ? '✅ activadas' : '❌ desactivadas'}`);
      }
      break;
    }

    case '/stop':
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ No tienes permiso para controlar la valvula.'); break; }
      tgEnviar(chatId, '🛑 <b>PARADA DE EMERGENCIA enviada al nodo.</b>');
      tgAlerta(`🛑 <b>PARADA DE EMERGENCIA</b> activada por ${usuarios.get(chatId)?.nombre || 'Admin'}`);
      break;

    case '/auto':
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ No tienes permiso para controlar la valvula.'); break; }
      camara1.modoAuto = true;
      tgEnviar(chatId, '✅ <b>Modo automatico PID activado.</b>');
      break;

    case '/manual':
      if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ No tienes permiso para controlar la valvula.'); break; }
      camara1.modoAuto = false;
      tgEnviar(chatId, '🔧 <b>Modo manual activado.</b>');
      break;

    default:
      if (cmd.startsWith('/setpoint')) {
        if (!esAdmin(chatId)) { tgEnviar(chatId, '⛔ No tienes permiso para cambiar el setpoint.'); break; }
        const parts = texto.trim().split(' ');
        const sp    = parseFloat(parts[1]);
        if (isNaN(sp) || sp <= 0 || sp >= camara1.presionP1) {
          tgEnviar(chatId, `⚠️ Setpoint invalido. Debe ser entre 1 y ${Math.floor(camara1.presionP1-1)} PSI.\nUso: /setpoint 20`);
        } else {
          camara1.setpoint = sp;
          tgEnviar(chatId, `✅ <b>Setpoint actualizado a ${sp} PSI</b>`);
          tgAlerta(`ℹ️ Setpoint cambiado a <b>${sp} PSI</b> por ${usuarios.get(chatId)?.nombre || 'Admin'}`);
        }
      } else if (rol === 'pending') {
        tgEnviar(chatId, `⛔ <b>Acceso pendiente de autorizacion.</b>\nEl administrador del sistema debe autorizarte.\nTu Chat ID es: <code>${chatId}</code>\n\nComparte este ID con el administrador.`);
      } else {
        tgEnviar(chatId, `Comando no reconocido. Escribe /ayuda para ver los comandos disponibles.`);
      }
  }
}

// ── Telegram: recibir mensajes (webhook) ──────────────────────────
app.post(`/webhook/${TG_TOKEN}`, (req, res) => {
  const body = req.body;
  res.sendStatus(200);
  if (!body.message) return;
  const chatId = body.message.chat.id;
  const texto  = body.message.text || '';
  if (texto) procesarComando(chatId, texto, req);
});

// ── Recibir datos del Gateway ESP32 ──────────────────────────────
app.post('/actualizar', (req, res) => {
  const d = req.body;
  if (!d) return res.status(400).json({ error: 'Sin datos' });

  const antBoya = camara1.boyaMojada;
  const antMov  = camara1.movimiento;

  camara1 = { ...camara1, ...d, ultimaActualizacion: new Date().toISOString() };

  if (typeof d.presionP2 === 'number') {
    camara1.historial.push(d.presionP2);
    if (camara1.historial.length > 30) camara1.historial.shift();
  }

  // Alertas automaticas por Telegram
  if (!antBoya && camara1.boyaMojada) {
    tgAlerta('🚨 <b>ALERTA INUNDACION</b>\nEl sensor de boya detecta agua en la Camara 1.\nRevisa inmediatamente.');
  }
  if (!antMov && camara1.movimiento) {
    tgAlerta('⚠️ <b>ALERTA MOVIMIENTO</b>\nSe detecto movimiento en la Camara 1.\nAcceso no autorizado.');
  }

  res.json({ ok: true });
});

// ── API para el dashboard ─────────────────────────────────────────
app.get('/datos', (req, res) => {
  res.json({ ...camara1, historial: camara1.historial });
});

// ── Endpoints IFTTT / Google Assistant ───────────────────────────
app.get('/camara1/presion', (req, res) => {
  res.send(`La presion aguas abajo es ${camara1.presionP2.toFixed(1)} PSI y aguas arriba es ${camara1.presionP1.toFixed(1)} PSI`);
});
app.get('/camara1/humedad', (req, res) => {
  res.send(`La humedad de la camara es ${camara1.humedad.toFixed(0)} por ciento`);
});
app.get('/camara1/temperatura', (req, res) => {
  res.send(`La temperatura de la camara es ${camara1.temperatura.toFixed(1)} grados centigrados`);
});
app.get('/camara1/estado', (req, res) => {
  const modo = camara1.modoAuto ? 'automatico' : 'manual';
  const boya = camara1.boyaMojada ? 'ALERTA inundacion detectada' : 'sin inundacion';
  res.send(`Camara 1: presion ${camara1.presionP2.toFixed(1)} PSI, setpoint ${camara1.setpoint.toFixed(0)} PSI, modo ${modo}, ${boya}`);
});
app.get('/camara1/boya', (req, res) => {
  res.send(camara1.boyaMojada ? 'ALERTA: el sensor de boya esta mojado, posible inundacion' : 'El sensor de boya esta seco, sin inundacion');
});

// ── Dashboard HTML ────────────────────────────────────────────────
app.get('/', (req, res) => {
  res.sendFile(__dirname + '/public/index.html');
});

// ── Registrar webhook de Telegram al arrancar ─────────────────────
function registrarWebhook() {
  const urlBase   = process.env.RAILWAY_PUBLIC_DOMAIN
    ? `https://${process.env.RAILWAY_PUBLIC_DOMAIN}`
    : 'https://autovrp-cloud-production.up.railway.app';
  const webhookUrl = `${urlBase}/webhook/${TG_TOKEN}`;

  const body = JSON.stringify({ url: webhookUrl });
  const req  = https.request({
    hostname: 'api.telegram.org',
    path: `/bot${TG_TOKEN}/setWebhook`,
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) }
  }, res => {
    let data = '';
    res.on('data', c => data += c);
    res.on('end', () => console.log('Webhook Telegram:', data));
  });
  req.write(body);
  req.end();
}

app.listen(PORT, () => {
  console.log(`AutoVRP servidor corriendo en puerto ${PORT}`);
  registrarWebhook();
});
