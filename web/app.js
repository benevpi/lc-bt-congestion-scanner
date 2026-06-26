// BLE Spectrum Monitor — Web Bluetooth wiring and app state.

import {
  NUM_CHANNELS,
  BUSY_THRESHOLD_DBM,
  createSpectrumChart,
  updateSpectrumChart,
  createHeatmapChart,
  updateHeatmapChart,
} from "./charts.js";

// --- Nordic UART Service UUIDs ---
const NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const NUS_RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // browser -> board (write)
const NUS_TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // board -> browser (notify)

// --- history ring buffer ---
// 5 min at 10 Hz = 3000 sweeps; also used to trim the 5-minute heatmap window.
const MAX_SWEEPS = 3000;
const HISTORY_WINDOW_MS = 5 * 60 * 1000;
const HEATMAP_MAX_COLS = 600; // keep the heatmap render cheap

// --- module state ---
const state = {
  device: null,
  server: null,
  rxChar: null,
  txChar: null,
  connected: false,
  sweeping: false,
  history: [], // [{ t, rssi[40], rxTime }]
  rxBuffer: new Uint8Array(0),
  decoder: new TextDecoder(),
  encoder: new TextEncoder(),
  spectrumChart: null,
  heatmapChart: null,
  pendingRender: false,
  latestSweep: null,
};

// --- DOM refs ---
const el = {};
function cacheDom() {
  for (const id of [
    "unsupported",
    "app",
    "status",
    "connectBtn",
    "startStopBtn",
    "intervalInput",
    "applyIntervalBtn",
    "avgRssi",
    "busyCount",
    "sweepRate",
    "bufferCount",
    "downloadCsvBtn",
    "thresholdLabel",
  ]) {
    el[id] = document.getElementById(id);
  }
}

// ------------------------------------------------------------ UI helpers

function setStatus(text, cls) {
  el.status.textContent = text;
  el.status.className = `pill ${cls}`;
}

function setConnectedUi(connected) {
  state.connected = connected;
  el.connectBtn.textContent = connected ? "Disconnect" : "Connect";
  el.startStopBtn.disabled = !connected;
  el.applyIntervalBtn.disabled = !connected;
  if (!connected) {
    state.sweeping = false;
    el.startStopBtn.textContent = "Start";
  }
}

// ------------------------------------------------------ Web Bluetooth

async function connect() {
  try {
    setStatus("Connecting…", "pill-connecting");
    state.device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }],
    });
    state.device.addEventListener("gattserverdisconnected", onDisconnected);

    state.server = await state.device.gatt.connect();
    const service = await state.server.getPrimaryService(NUS_SERVICE);
    state.txChar = await service.getCharacteristic(NUS_TX_CHAR);
    state.rxChar = await service.getCharacteristic(NUS_RX_CHAR);

    await state.txChar.startNotifications();
    state.txChar.addEventListener("characteristicvaluechanged", onNotify);

    setStatus(`Connected to ${state.device.name || "device"}`, "pill-on");
    setConnectedUi(true);
    el.downloadCsvBtn.disabled = false;
  } catch (err) {
    console.error("Connect failed:", err);
    setStatus("Disconnected", "pill-off");
    setConnectedUi(false);
    if (err && err.name !== "NotFoundError") {
      // NotFoundError == user dismissed the chooser; don't alarm them.
      alert(`Connection failed: ${err.message || err}`);
    }
  }
}

function disconnect() {
  if (state.device && state.device.gatt.connected) {
    state.device.gatt.disconnect();
  } else {
    onDisconnected();
  }
}

function onDisconnected() {
  setStatus("Disconnected", "pill-off");
  setConnectedUi(false);
  state.server = null;
  state.rxChar = null;
  state.txChar = null;
  state.rxBuffer = new Uint8Array(0);
}

async function sendCommand(cmd) {
  if (!state.rxChar) return;
  const data = state.encoder.encode(cmd + "\n");
  try {
    await state.rxChar.writeValueWithResponse(data);
  } catch (err) {
    console.error(`Failed to send "${cmd}":`, err);
  }
}

// ------------------------------------------------- incoming data parsing

function onNotify(event) {
  const chunk = new Uint8Array(event.target.value.buffer);

  // Append to the reassembly buffer.
  const merged = new Uint8Array(state.rxBuffer.length + chunk.length);
  merged.set(state.rxBuffer, 0);
  merged.set(chunk, state.rxBuffer.length);
  state.rxBuffer = merged;

  // Split on newline; each complete line is a message.
  let start = 0;
  for (let i = 0; i < state.rxBuffer.length; i++) {
    if (state.rxBuffer[i] === 0x0a /* \n */) {
      const lineBytes = state.rxBuffer.subarray(start, i);
      const line = state.decoder.decode(lineBytes).trim();
      if (line) handleLine(line);
      start = i + 1;
    }
  }
  // Keep the trailing partial line for next time.
  state.rxBuffer = state.rxBuffer.slice(start);
}

function handleLine(line) {
  if (line === "PONG") {
    console.log("PONG");
    return;
  }
  if (line.startsWith("ERR")) {
    console.warn("Board error:", line);
    return;
  }
  if (line[0] !== "{") {
    console.log("Board:", line);
    return;
  }

  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    console.warn("Bad JSON line:", line);
    return;
  }
  if (!Array.isArray(msg.rssi) || msg.rssi.length !== NUM_CHANNELS) {
    console.warn("Sweep with unexpected shape:", msg);
    return;
  }

  recordSweep(msg);
}

// --------------------------------------------------------- history & render

function recordSweep(msg) {
  const record = { t: msg.t, rssi: msg.rssi, rxTime: performance.now() };
  state.history.push(record);
  state.latestSweep = record;

  // Trim by both count and the 5-minute time window.
  if (state.history.length > MAX_SWEEPS) {
    state.history.splice(0, state.history.length - MAX_SWEEPS);
  }
  const cutoff = performance.now() - HISTORY_WINDOW_MS;
  while (state.history.length && state.history[0].rxTime < cutoff) {
    state.history.shift();
  }

  scheduleRender();
}

// Coalesce redraws to one per animation frame so bursts of notifications do
// not cause redundant chart updates.
function scheduleRender() {
  if (state.pendingRender) return;
  state.pendingRender = true;
  requestAnimationFrame(() => {
    state.pendingRender = false;
    render();
  });
}

function render() {
  if (!state.latestSweep) return;

  updateSpectrumChart(state.spectrumChart, state.latestSweep.rssi);

  // Downsample columns for the heatmap if history is long.
  const hist = state.history;
  let cols = hist;
  if (hist.length > HEATMAP_MAX_COLS) {
    const step = Math.ceil(hist.length / HEATMAP_MAX_COLS);
    cols = hist.filter((_, i) => i % step === 0);
  }
  updateHeatmapChart(state.heatmapChart, cols);

  updateSummary();
}

function updateSummary() {
  const sweep = state.latestSweep;
  if (!sweep) return;

  const sum = sweep.rssi.reduce((a, b) => a + b, 0);
  const avg = sum / sweep.rssi.length;
  el.avgRssi.textContent = `${avg.toFixed(1)} dBm`;

  const busy = sweep.rssi.filter((v) => v > BUSY_THRESHOLD_DBM).length;
  el.busyCount.textContent = `${busy} / ${NUM_CHANNELS}`;

  // Sweep rate from inter-arrival times over the last few sweeps.
  const n = state.history.length;
  if (n >= 2) {
    const span = state.history[n - 1].rxTime - state.history[0].rxTime;
    const rate = span > 0 ? ((n - 1) / span) * 1000 : 0;
    el.sweepRate.textContent = `${rate.toFixed(2)} Hz`;
  } else {
    el.sweepRate.textContent = "—";
  }

  el.bufferCount.textContent = String(n);
}

// --------------------------------------------------------------- CSV export

function downloadCsv() {
  const header = ["timestamp_ms"];
  for (let i = 0; i < NUM_CHANNELS; i++) header.push(`ch${i}`);
  const rows = [header.join(",")];

  for (const s of state.history) {
    rows.push([s.t, ...s.rssi].join(","));
  }

  const blob = new Blob([rows.join("\n")], { type: "text/csv" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  a.download = `ble-spectrum-${stamp}.csv`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

// ------------------------------------------------------------------- wiring

function wireUi() {
  el.connectBtn.addEventListener("click", () => {
    if (state.connected) disconnect();
    else connect();
  });

  el.startStopBtn.addEventListener("click", async () => {
    if (!state.connected) return;
    if (state.sweeping) {
      await sendCommand("STOP");
      state.sweeping = false;
      el.startStopBtn.textContent = "Start";
    } else {
      await sendCommand("START");
      state.sweeping = true;
      el.startStopBtn.textContent = "Stop";
    }
  });

  el.applyIntervalBtn.addEventListener("click", async () => {
    let ms = parseInt(el.intervalInput.value, 10);
    if (!Number.isFinite(ms) || ms < 100) ms = 100;
    el.intervalInput.value = String(ms);
    await sendCommand(`INTERVAL ${ms}`);
  });

  el.downloadCsvBtn.addEventListener("click", downloadCsv);
}

function init() {
  cacheDom();

  // Feature-detect Web Bluetooth before doing anything else.
  if (!navigator.bluetooth) {
    el.app.classList.add("hidden");
    el.unsupported.classList.remove("hidden");
    return;
  }

  el.thresholdLabel.textContent = String(BUSY_THRESHOLD_DBM);
  state.spectrumChart = createSpectrumChart(
    document.getElementById("spectrumChart")
  );
  state.heatmapChart = createHeatmapChart(
    document.getElementById("heatmapChart")
  );

  wireUi();
}

document.addEventListener("DOMContentLoaded", init);
