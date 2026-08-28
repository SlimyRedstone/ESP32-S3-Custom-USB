/*
 * WebUSB client for the ESP32-S3 composite device.
 *
 * Talks to the vendor-specific interface only. The CDC function is left to the
 * operating system, so the COM port keeps working while this page is open.
 *
 * Chrome and Edge only. On Windows the vendor function must have WinUSB bound
 * to it, which the firmware's MS OS 2.0 descriptors arrange automatically.
 */

const VID = 0x303A;
const PID = 0x4001;
const PACKET_SIZE = 64;

/*
 * Replies can exceed one packet, so read a whole transfer at a time. The
 * device terminates every transfer with a short packet, which ends transferIn.
 */
const READ_SIZE = 512;

/* Matches JSON_BUF_MAX in the firmware. */
const JSON_MAX = 512;

const el = (id) => document.getElementById(id);

const ui = {
  connect: el('connect'),
  disconnect: el('disconnect'),
  status: el('status'),
  unsupported: el('unsupported'),
  wheel: el('wheel'),
  marker: el('marker'),
  value: el('value'),
  hex: el('hex'),
  swatch: el('swatch'),
  live: el('live'),
  sendLed: el('send-led'),
  getLed: el('get-led'),
  config: el('config'),
  getConfig: el('get-config'),
  setConfig: el('set-config'),
  configStatus: el('config-status'),
  msg: el('msg'),
  msgForm: el('msg-form'),
  sendMsg: el('send-msg'),
  log: el('log'),
  clearLog: el('clear-log'),
  showHeartbeat: el('show-heartbeat'),
};

let device = null;
let ifaceNum = null;
let epOut = null;
let epIn = null;
let reading = false;

/* ------------------------------------------------------------------ log -- */

function log(kind, text) {
  const time = new Date().toLocaleTimeString([], { hour12: false });
  const line = document.createElement('div');
  line.innerHTML =
    `<span class="t">${time}</span> <span class="${kind}">${escapeHtml(text)}</span>`;
  ui.log.appendChild(line);
  ui.log.scrollTop = ui.log.scrollHeight;

  while (ui.log.childElementCount > 500) {
    ui.log.removeChild(ui.log.firstChild);
  }
}

function escapeHtml(s) {
  return s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[c]);
}

/* --------------------------------------------------------------- colour -- */

let hue = 0;          /* 0..360 */
let sat = 1;          /* 0..1   */
let val = 1;          /* 0..1   */

function hsvToRgb(h, s, v) {
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  let r = 0, g = 0, b = 0;

  if (h < 60)       { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else              { r = c; b = x; }

  return [
    Math.round((r + m) * 255),
    Math.round((g + m) * 255),
    Math.round((b + m) * 255),
  ];
}

function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const d = max - min;

  let h = 0;
  if (d !== 0) {
    if (max === r)      h = 60 * (((g - b) / d) % 6);
    else if (max === g) h = 60 * ((b - r) / d + 2);
    else                h = 60 * ((r - g) / d + 4);
  }
  if (h < 0) h += 360;

  return [h, max === 0 ? 0 : d / max, max];
}

function currentHex() {
  const [r, g, b] = hsvToRgb(hue, sat, val);
  return ((r << 16) | (g << 8) | b).toString(16).padStart(6, '0').toUpperCase();
}

/* Paint an HSV disc: hue around the circumference, saturation along the radius. */
function drawWheel() {
  const ctx = ui.wheel.getContext('2d');
  const w = ui.wheel.width;
  const h = ui.wheel.height;
  const cx = w / 2;
  const cy = h / 2;
  const radius = Math.min(cx, cy) - 2;

  const img = ctx.createImageData(w, h);
  const px = img.data;

  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const dx = x - cx;
      const dy = y - cy;
      const dist = Math.sqrt(dx * dx + dy * dy);
      const i = (y * w + x) * 4;

      if (dist > radius) {
        px[i + 3] = 0;
        continue;
      }

      let angle = (Math.atan2(dy, dx) * 180) / Math.PI + 90;
      if (angle < 0) angle += 360;

      const [r, g, b] = hsvToRgb(angle, Math.min(dist / radius, 1), val);
      px[i] = r;
      px[i + 1] = g;
      px[i + 2] = b;

      /* Feather the last pixel of the rim so the edge is not jagged. */
      px[i + 3] = dist > radius - 1 ? Math.round((radius - dist) * 255) : 255;
    }
  }

  ctx.putImageData(img, 0, 0);
}

function moveMarker() {
  const radius = ui.wheel.width / 2 - 2;
  const angle = ((hue - 90) * Math.PI) / 180;
  const r = sat * radius;
  ui.marker.style.left = `${ui.wheel.width / 2 + r * Math.cos(angle)}px`;
  ui.marker.style.top = `${ui.wheel.height / 2 + r * Math.sin(angle)}px`;
  ui.marker.style.background = `#${currentHex()}`;
}

function refresh({ redraw = false } = {}) {
  if (redraw) drawWheel();
  moveMarker();
  const hex = currentHex();
  ui.swatch.style.background = `#${hex}`;
  if (document.activeElement !== ui.hex) {
    ui.hex.value = `#${hex}`;
  }
}

function pickFromEvent(event) {
  const rect = ui.wheel.getBoundingClientRect();
  const cx = rect.width / 2;
  const cy = rect.height / 2;
  const dx = event.clientX - rect.left - cx;
  const dy = event.clientY - rect.top - cy;
  const radius = Math.min(cx, cy) - 2;
  const dist = Math.sqrt(dx * dx + dy * dy);

  let angle = (Math.atan2(dy, dx) * 180) / Math.PI + 90;
  if (angle < 0) angle += 360;

  hue = angle;
  sat = Math.min(dist / radius, 1);
  refresh();
}

/* ------------------------------------------------------------------ usb -- */

function setConnected(connected) {
  ui.status.textContent = connected ? 'connected' : 'disconnected';
  ui.status.className = `badge ${connected ? 'on' : 'off'}`;
  ui.connect.hidden = connected;
  ui.disconnect.hidden = !connected;
  ui.sendLed.disabled = !connected;
  ui.sendMsg.disabled = !connected;
  ui.getLed.disabled = !connected;
  ui.getConfig.disabled = !connected;
  ui.setConfig.disabled = !connected;
}

async function connect() {
  try {
    device = await navigator.usb.requestDevice({
      filters: [{ vendorId: VID, productId: PID }],
    });
  } catch {
    log('er', 'no device selected');
    return;
  }

  try {
    await device.open();
    if (device.configuration === null) {
      await device.selectConfiguration(1);
    }

    const found = findVendorInterface(device.configuration);
    if (!found) {
      throw new Error('no vendor interface with bulk IN and OUT');
    }
    ({ ifaceNum, epOut, epIn } = found);

    await device.claimInterface(ifaceNum);

    setConnected(true);
    log('ev', `connected: interface ${ifaceNum}, OUT ep${epOut}, IN ep${epIn}`);

    readLoop();
  } catch (err) {
    log('er', `connect failed: ${err.message}`);
    await disconnect();
  }
}

/* The device is composite, so skip the CDC interfaces and match class 0xFF. */
function findVendorInterface(configuration) {
  for (const iface of configuration.interfaces) {
    const alt = iface.alternates[0];
    if (alt.interfaceClass !== 0xFF) continue;

    const out = alt.endpoints.find((e) => e.direction === 'out' && e.type === 'bulk');
    const inp = alt.endpoints.find((e) => e.direction === 'in' && e.type === 'bulk');
    if (out && inp) {
      return {
        ifaceNum: iface.interfaceNumber,
        epOut: out.endpointNumber,
        epIn: inp.endpointNumber,
      };
    }
  }
  return null;
}

async function disconnect() {
  reading = false;
  const d = device;
  device = null;

  if (d) {
    try {
      if (ifaceNum !== null) await d.releaseInterface(ifaceNum);
      await d.close();
    } catch { /* already gone */ }
  }

  ifaceNum = epOut = epIn = null;
  setConnected(false);
  log('ev', 'disconnected');
}

async function send(text) {
  if (!device) return;

  const bytes = new TextEncoder().encode(text);
  if (bytes.length > PACKET_SIZE) {
    log('er', `too long: ${bytes.length} bytes, max ${PACKET_SIZE}`);
    return;
  }

  try {
    await device.transferOut(epOut, bytes);
    log('tx', `-> ${text}`);
  } catch (err) {
    log('er', `send failed: ${err.message}`);
    await disconnect();
  }
}

async function readLoop() {
  reading = true;

  while (reading && device) {
    let result;
    try {
      result = await device.transferIn(epIn, READ_SIZE);
    } catch (err) {
      if (reading) {
        log('er', `read failed: ${err.message}`);
        await disconnect();
      }
      return;
    }

    if (result.status !== 'ok' || !result.data || result.data.byteLength === 0) {
      continue;
    }

    /* The DataView is a window onto a larger buffer, so respect its bounds. */
    handlePacket(new Uint8Array(
      result.data.buffer, result.data.byteOffset, result.data.byteLength));
  }
}

function handlePacket(bytes) {
  /* 0x5A followed by a little-endian uint32 counter. Still binary, not JSON. */
  if (bytes[0] === 0x5A && bytes.length >= 5) {
    if (ui.showHeartbeat.checked) {
      const count = bytes[1] | (bytes[2] << 8) | (bytes[3] << 16) | (bytes[4] << 24);
      log('rx', `heartbeat ${count >>> 0}`);
    }
    return;
  }

  const text = new TextDecoder().decode(bytes);

  /* Replies are JSON; asynchronous events such as the button are plain text. */
  if (text.startsWith('{')) {
    let obj;
    try {
      obj = JSON.parse(text);
    } catch {
      log('er', `<- unparseable reply ${text}`);
      return;
    }
    handleReply(obj, text);
    return;
  }

  log('ev', `<- ${text}`);
}

function handleReply(obj, raw) {
  if (obj.ok === false) {
    log('er', `<- error: ${obj.error}`);
    setConfigStatus(obj.error, false);
    return;
  }
  if (obj.ok === true) {
    log('rx', '<- ok');
    return;
  }

  /* {"led":"ABCDEF"} -- adopt the reported colour so the wheel matches. */
  if (typeof obj.led === 'string') {
    log('rx', `<- led ${obj.led}`);
    applyHex(obj.led);
    return;
  }

  /* {"interrupt":{...}} -- unprompted GPIO report, not a reply to anything. */
  if (obj.interrupt && typeof obj.interrupt === 'object') {
    const i = obj.interrupt;
    log('ev', `<- interrupt gpio=${i.gpio} state=${i.state} "${i.message ?? ''}"`);
    return;
  }

  /* {"config":{...}} -- fill the editor with what the device actually holds. */
  if (obj.config && typeof obj.config === 'object') {
    log('rx', `<- config ${JSON.stringify(obj.config)}`);
    ui.config.value = JSON.stringify(obj.config, null, 2);
    setConfigStatus('loaded from device', true);
    return;
  }

  log('rx', `<- ${raw}`);
}

/* ----------------------------------------------------------------- wire -- */

function sendJson(obj) {
  const text = JSON.stringify(obj);
  if (new TextEncoder().encode(text).length > JSON_MAX) {
    log('er', `payload too large, max ${JSON_MAX} bytes`);
    return false;
  }
  send(text);
  return true;
}

function sendLed() {
  sendJson({ set: { led: currentHex() } });
}

function getLed() {
  sendJson({ get: 'led' });
}

function getConfig() {
  sendJson({ get: 'config' });
}

function setConfig() {
  const raw = ui.config.value.trim();
  if (!raw) {
    setConfigStatus('nothing to send', false);
    return;
  }

  let obj;
  try {
    obj = JSON.parse(raw);
  } catch (err) {
    setConfigStatus(`not valid JSON: ${err.message}`, false);
    return;
  }
  if (typeof obj !== 'object' || Array.isArray(obj)) {
    setConfigStatus('expected a JSON object', false);
    return;
  }

  if (sendJson({ set: { config: obj } })) {
    setConfigStatus('sent', true);
  }
}

function setConfigStatus(message, ok) {
  ui.configStatus.textContent = message;
  ui.configStatus.className = `hint ${ok ? 'status-ok' : 'status-err'}`;
}

/* Point the wheel at a colour the device reported. */
function applyHex(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex);
  if (!m) return;

  const n = parseInt(m[1], 16);
  [hue, sat, val] = rgbToHsv((n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
  ui.value.value = String(Math.round(val * 100));
  refresh({ redraw: true });
}

let liveTimer = null;
function sendLedLive() {
  if (!ui.live.checked || !device) return;
  if (liveTimer) return;
  liveTimer = setTimeout(() => {
    liveTimer = null;
    sendLed();
  }, 60);
}

/* ---------------------------------------------------------------- events -- */

ui.wheel.addEventListener('pointerdown', (e) => {
  ui.wheel.setPointerCapture(e.pointerId);
  pickFromEvent(e);
  sendLedLive();
});

ui.wheel.addEventListener('pointermove', (e) => {
  if (!ui.wheel.hasPointerCapture(e.pointerId)) return;
  pickFromEvent(e);
  sendLedLive();
});

ui.wheel.addEventListener('pointerup', (e) => {
  ui.wheel.releasePointerCapture(e.pointerId);
  if (ui.live.checked) sendLed();
});

ui.value.addEventListener('input', () => {
  val = Number(ui.value.value) / 100;
  refresh({ redraw: true });
  sendLedLive();
});

ui.hex.addEventListener('input', () => {
  applyHex(ui.hex.value.trim());
});

ui.sendLed.addEventListener('click', sendLed);

ui.msgForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = ui.msg.value;
  if (!text) return;
  sendJson({ set: { message: text } });
});

ui.getLed.addEventListener('click', getLed);
ui.getConfig.addEventListener('click', getConfig);
ui.setConfig.addEventListener('click', setConfig);

ui.connect.addEventListener('click', connect);
ui.disconnect.addEventListener('click', disconnect);
ui.clearLog.addEventListener('click', () => { ui.log.textContent = ''; });

/* Unplugging the device mid-session. */
if (navigator.usb) {
  navigator.usb.addEventListener('disconnect', (e) => {
    if (device && e.device === device) {
      log('er', 'device unplugged');
      disconnect();
    }
  });
} else {
  ui.unsupported.hidden = false;
  ui.connect.disabled = true;
}

setConnected(false);
refresh({ redraw: true });
