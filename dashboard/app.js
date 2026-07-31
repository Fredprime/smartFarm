// ============================================================
//  app.js — SmartFarm IoT Dashboard Client
//  Connects to ESP32 WebSocket, renders live charts & controls
// ============================================================

'use strict';

// ----------------------------------------------------------
// Configuration — UPDATE WITH YOUR ESP32 IP ADDRESS
// ----------------------------------------------------------
const CONFIG = {
  // Replace with your ESP32's IP address from Serial Monitor
  ESP32_IP:   '192.168.1.100',   // <-- CHANGE THIS FOR LOCAL DEV
  WS_PORT:    81,
  MAX_LOG:    100,               // Max log table rows
  MAX_CHART:  20,                // Data points per chart
  RECONNECT_DELAY: 3000,        // ms between reconnect attempts
  DEMO_MODE:  false,            // false by default for live hardware monitoring
};

// ----------------------------------------------------------
// State
// ----------------------------------------------------------
const state = {
  ws:           null,
  connected:    false,
  autoMode:     true,
  pumpState:    false,
  data: {
    soil:       [],
    temp:       [],
    humid:      [],
    pump:       [],
    labels:     [],
  },
  logRows:      [],
  reconnectTimer: null,
  demoInterval:   null,
  lastData:       null,
};

// ----------------------------------------------------------
// WebSocket Setup
// ----------------------------------------------------------
function connectWebSocket() {
  const url = `ws://${CONFIG.ESP32_IP}:${CONFIG.WS_PORT}`;
  updateConnectionBadge('connecting');

  try {
    state.ws = new WebSocket(url);
  } catch (err) {
    console.error('[WS] Failed to create WebSocket:', err);
    scheduleReconnect();
    return;
  }

  state.ws.onopen = () => {
    console.log('[WS] Connected to ESP32');
    state.connected = true;
    updateConnectionBadge('connected');
    stopDemo();
    clearReconnectTimer();
    // Send browser settings/KPI thresholds to ESP32 on connection
    sendSettingsToESP32();
  };

  state.ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      processData(data);
    } catch (err) {
      console.error('[WS] JSON parse error:', err);
    }
  };

  state.ws.onclose = (event) => {
    console.warn('[WS] Disconnected:', event.code, event.reason);
    state.connected = false;
    updateConnectionBadge('disconnected');
    if (CONFIG.DEMO_MODE) startDemo();
    scheduleReconnect();
  };

  state.ws.onerror = (err) => {
    console.error('[WS] Error:', err);
    state.connected = false;
    updateConnectionBadge('disconnected');
  };
}

// ----------------------------------------------------------
// MQTT Configuration — Remote Cross-Network Connectivity
// ----------------------------------------------------------
const MQTT_CONFIG = {
  brokerWssUrl: 'wss://broker.hivemq.com:8884/mqtt',
  topicTelemetry: 'smartfarm/telemetry',
  topicControl:   'smartfarm/control',
  clientId:       'SmartFarm_Dashboard_' + Math.random().toString(16).slice(2, 8)
};

let mqttClient = null;

function initMQTT() {
  if (typeof mqtt === 'undefined') {
    console.warn('[MQTT] mqtt.js CDN not loaded — skipping MQTT init');
    return;
  }

  console.log('[MQTT] Connecting to remote broker:', MQTT_CONFIG.brokerWssUrl);

  try {
    mqttClient = mqtt.connect(MQTT_CONFIG.brokerWssUrl, {
      clientId: MQTT_CONFIG.clientId,
      clean: true,
      reconnectPeriod: 5000,
      connectTimeout: 10000
    });

    mqttClient.on('connect', () => {
      console.log('[MQTT] Connected to cloud broker');
      mqttClient.subscribe(MQTT_CONFIG.topicTelemetry, { qos: 0 }, (err) => {
        if (!err) {
          console.log('[MQTT] Subscribed to telemetry topic:', MQTT_CONFIG.topicTelemetry);
          if (!state.connected) {
            updateConnectionBadge('connecting', 'Cloud Connected · Awaiting ESP32 Telemetry…');
          }
        } else {
          console.error('[MQTT] Subscribe error:', err);
        }
      });
    });

    mqttClient.on('message', (topic, payload) => {
      if (topic === MQTT_CONFIG.topicTelemetry) {
        try {
          const data = JSON.parse(payload.toString());
          state.connected = true;
          stopDemo();
          updateConnectionBadge('connected', 'ESP32 Connected (Cloud)');
          processData(data);
        } catch (e) {
          console.error('[MQTT] JSON parse error:', e);
        }
      }
    });

    mqttClient.on('offline', () => {
      console.warn('[MQTT] Client offline — waiting for reconnect…');
      if (!state.ws || state.ws.readyState !== WebSocket.OPEN) {
        updateConnectionBadge('disconnected', 'Cloud Offline · Retrying…');
      }
    });
  } catch (err) {
    console.error('[MQTT] Init error:', err);
  }
}

function publishMQTTCommand(payload) {
  if (mqttClient && mqttClient.connected) {
    const msg = JSON.stringify(payload);
    mqttClient.publish(MQTT_CONFIG.topicControl, msg, { qos: 1 });
    console.log('[MQTT] Command published:', msg);
    return true;
  }
  return false;
}

function sendCommand(payload) {
  let sent = false;

  // 1. Send via local WebSocket if connected
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify(payload));
    sent = true;
  }

  // 2. Also publish via MQTT for remote cross-network control
  if (publishMQTTCommand(payload)) {
    sent = true;
  }

  if (!sent) {
    console.warn('[CMD] Not connected to WS or MQTT — fallback');
    if (CONFIG.DEMO_MODE) {
      if (payload.action === 'pump_on')        state.pumpState = true;
      else if (payload.action === 'pump_off')  state.pumpState = false;
      else if (payload.action === 'set_auto')  state.autoMode  = payload.value;
      updatePumpUI(state.pumpState);
      updateModeUI(state.autoMode);
    }
  }
}

function scheduleReconnect() {
  clearReconnectTimer();
  state.reconnectTimer = setTimeout(() => {
    console.log('[WS] Attempting reconnect…');
    connectWebSocket();
  }, CONFIG.RECONNECT_DELAY);
}

function clearReconnectTimer() {
  if (state.reconnectTimer) {
    clearTimeout(state.reconnectTimer);
    state.reconnectTimer = null;
  }
}

// ----------------------------------------------------------
// Connection Badge UI
// ----------------------------------------------------------
function updateConnectionBadge(status, customText = null) {
  const badge = document.getElementById('connection-badge');
  const dot   = document.getElementById('conn-dot');
  const text  = document.getElementById('conn-text');

  if (!badge || !dot || !text) return; // Guard against missing elements

  badge.className = `connection-badge ${status}`;

  const labels = {
    connecting:   'Connecting…',
    connected:    'Connected',
    disconnected: 'Disconnected',
    demo:         'Demo Mode',
  };

  text.textContent = customText || labels[status] || status;
  
  // Update class and styling cleanly
  dot.className = 'dot' + (status === 'connecting' ? ' pulse' : '');
  
  // Custom styling to match theme color schemes
  if (status === 'demo') {
    badge.style.borderColor = 'rgba(251,191,36,0.35)';
    badge.style.color       = '#fbbf24';
    dot.style.background    = '#fbbf24';
    dot.style.animation     = 'none';
  } else if (status === 'connecting') {
    badge.style.borderColor = 'rgba(34,197,94,0.15)';
    badge.style.color       = 'var(--text-secondary)';
    dot.style.background    = 'var(--green-400)';
    dot.style.animation     = 'pulse 1.5s infinite ease-in-out';
  } else if (status === 'connected') {
    badge.style.borderColor = 'rgba(34,197,94,0.3)';
    badge.style.color       = 'var(--green-400)';
    dot.style.background    = 'var(--green-400)';
    dot.style.animation     = 'none';
  } else {
    // disconnected
    badge.style.borderColor = 'rgba(244,63,94,0.35)';
    badge.style.color       = 'var(--rose-500)';
    dot.style.background    = 'var(--rose-500)';
    dot.style.animation     = 'none';
  }

  document.getElementById('footer-ip').textContent =
    `ws://${CONFIG.ESP32_IP}:${CONFIG.WS_PORT}`;
}

// ----------------------------------------------------------
// Supabase Cloud Logging State
// ----------------------------------------------------------
let lastCloudLogTime = 0;
let lastLoggedPumpState = null;
let lastLoggedAlertCount = -1;

async function logSensorDataToCloud(data) {
  const sb = initSupabase();
  if (!sb) return;

  const now = Date.now();
  const interval = (typeof SUPABASE_CONFIG !== 'undefined' && SUPABASE_CONFIG.cloudLogIntervalMs) || 60000;
  if (now - lastCloudLogTime < interval) return;
  lastCloudLogTime = now;

  try {
    const { error } = await sb.from('sensor_readings').insert([{
      temperature: data.temperature,
      humidity: data.humidity,
      soil_moisture: data.soilMoisture,
      soil_raw: data.soilRaw,
      tank_full: data.tankFull,
      pump_state: data.pumpState,
      auto_mode: data.autoMode,
      alerts: data.alerts || []
    }]);
    if (error) console.warn('[Supabase Log] Sensor log failed:', error.message);
    else console.log('[Supabase Log] Sensor reading logged to cloud');
  } catch (err) {
    console.error('[Supabase Log] Error logging sensor data:', err);
  }
}

async function logPumpEventToCloud(eventType, pumpState) {
  const sb = initSupabase();
  if (!sb) return;

  try {
    const { error } = await sb.from('pump_events').insert([{
      event_type: eventType,
      pump_state: pumpState,
      triggered_by: state.autoMode ? 'auto' : 'user'
    }]);
    if (error) console.warn('[Supabase Log] Pump event log failed:', error.message);
    else console.log(`[Supabase Log] Pump event (${eventType}) logged`);
  } catch (err) {
    console.error('[Supabase Log] Error logging pump event:', err);
  }
}

async function logAlertsToCloud(alerts, count) {
  if (count === lastLoggedAlertCount) return;
  lastLoggedAlertCount = count;
  if (count === 0) return;

  const sb = initSupabase();
  if (!sb) return;

  try {
    const { error } = await sb.from('alerts_log').insert([{
      alert_count: count,
      alerts: alerts
    }]);
    if (error) console.warn('[Supabase Log] Alerts log failed:', error.message);
    else console.log('[Supabase Log] Alert batch logged to cloud');
  } catch (err) {
    console.error('[Supabase Log] Error logging alerts:', err);
  }
}

async function logoutSupabase() {
  const sb = initSupabase();
  if (sb) {
    await sb.auth.signOut();
  }
  window.location.href = 'login.html';
}

// ----------------------------------------------------------
// Data Processing
// ----------------------------------------------------------
function processData(data) {
  if (lastLoggedPumpState !== null && lastLoggedPumpState !== data.pumpState) {
    logPumpEventToCloud(data.pumpState ? (data.autoMode ? 'auto_on' : 'manual_on') : (data.autoMode ? 'auto_off' : 'manual_off'), data.pumpState);
  }
  lastLoggedPumpState = data.pumpState;

  state.lastData   = data;
  state.pumpState  = data.pumpState;
  state.autoMode   = data.autoMode;

  // Update all UI components
  updateSensorCards(data);
  updatePumpUI(data.pumpState);
  updateModeUI(data.autoMode);
  updateTankUI(data.tankFull);
  updateSDStatus(data.sdAvailable, data.sdLogging);
  updateAlerts(data.alerts || [], data.alertCount || 0);
  updateCharts(data);
  appendLog(data);

  // Cloud logging
  logSensorDataToCloud(data);
  logAlertsToCloud(data.alerts || [], data.alertCount || 0);
}

// ----------------------------------------------------------
// Sensor Cards
// ----------------------------------------------------------
function updateSensorCards(data) {
  // --- Soil Moisture ---
  const soil = data.soilMoisture;
  animateValue('soil-value', soil);
  updateMoistureRing(soil);
  document.getElementById('soil-status').textContent = getSoilStatus(soil);

  // --- Temperature ---
  const temp = data.temperature.toFixed(1);
  animateElement('temp-value');
  document.getElementById('temp-value').innerHTML =
    `${temp}<span class="card-unit">°C</span>`;
  document.getElementById('temp-status').textContent = getTempStatus(data.temperature);
  // Bar: map 0–50°C to 0–100%
  const tempPct = Math.min(100, Math.max(0, (data.temperature / 50) * 100));
  document.getElementById('temp-bar').style.width = tempPct + '%';

  // --- Humidity ---
  const hum = data.humidity.toFixed(1);
  animateElement('humid-value');
  document.getElementById('humid-value').innerHTML =
    `${hum}<span class="card-unit">%</span>`;
  document.getElementById('humid-status').textContent = getHumidStatus(data.humidity);
  document.getElementById('humid-bar').style.width = data.humidity + '%';

  // --- RTC ---
  document.getElementById('rtc-time').textContent = data.timestamp || '--:--:--';
  document.getElementById('rtc-date').textContent = data.datestamp || '----·--·--';
}

function updateMoistureRing(pct) {
  const ring = document.getElementById('moisture-ring-progress');
  const circumference = 2 * Math.PI * 52; // r=52 → ~326.7
  const offset = circumference - (pct / 100) * circumference;
  ring.style.strokeDashoffset = offset;

  // Color shift: red (dry) → yellow → green (wet)
  let color;
  if (pct < 25)       color = '#f43f5e'; // Rose — drought
  else if (pct < 45)  color = '#fbbf24'; // Amber — low
  else if (pct < 70)  color = '#4ade80'; // Green — good
  else                color = '#22d3ee'; // Cyan — saturated
  ring.style.stroke = color;

  // Also update the card icon glow
  document.getElementById('soil-value').style.color = color;
}

function getSoilStatus(pct) {
  if (pct < 20)  return '🔴 Critical — Extreme drought risk';
  if (pct < 30)  return '🟠 Low — Irrigation recommended';
  if (pct < 50)  return '🟡 Moderate — Monitoring required';
  if (pct < 70)  return '🟢 Good — Optimal moisture level';
  if (pct < 85)  return '💧 High — Well irrigated';
  return              '🔵 Very High — Possible waterlogging';
}

function getTempStatus(t) {
  if (t < 4)   return '❄️ Frost risk — Protect crops';
  if (t < 15)  return '🌥️ Cool — Slow plant growth';
  if (t < 25)  return '✅ Optimal growing temperature';
  if (t < 35)  return '🌤️ Warm — Monitor evaporation';
  return            '🔥 Heat stress — Irrigate more';
}

function getHumidStatus(h) {
  if (h < 30)  return '🏜️ Very dry — Increase irrigation';
  if (h < 50)  return '🌤️ Dry — Normal evapotranspiration';
  if (h < 70)  return '✅ Comfortable humidity';
  if (h < 85)  return '💧 Humid — Monitor for disease';
  return            '⚠️ High — Fungal risk elevated';
}

// ----------------------------------------------------------
// Pump UI
// ----------------------------------------------------------
function updatePumpUI(on) {
  const led    = document.getElementById('pump-led');
  const txt    = document.getElementById('pump-status-text');
  const btnOn  = document.getElementById('btn-pump-on');
  const btnOff = document.getElementById('btn-pump-off');
  const info   = document.getElementById('pump-info');

  if (!led || !txt || !btnOn || !btnOff) return;

  const isAuto = state.autoMode;

  // LED indicator — always reflects actual relay state
  led.className = 'pump-led' + (on ? ' active' : '');

  // Status text — show mode context clearly
  if (isAuto) {
    txt.textContent = on ? 'AUTO — PUMP ACTIVE' : 'AUTO — STANDBY';
  } else {
    txt.textContent = on ? 'PUMP ACTIVE' : 'PUMP DEACTIVATED';
  }
  txt.className = 'pump-status-text ' + (on ? 'on' : 'off');

  // Buttons: in auto mode both are disabled (auto controls the pump)
  if (isAuto) {
    btnOn.disabled  = true;
    btnOff.disabled = true;
    btnOn.title     = 'Switch to Manual mode to control the pump';
    btnOff.title    = 'Switch to Manual mode to control the pump';
  } else {
    btnOn.disabled  = on;   // Start disabled when pump already on
    btnOff.disabled = !on;  // Stop disabled when pump already off
    btnOn.title     = '';
    btnOff.title    = '';
  }

  // Footer context line
  if (info) {
    if (isAuto) {
      info.innerHTML = `Auto irrigation active &middot; trigger range: moisture &lt; <span id="threshold-low">${state.settings?.moistureLow ?? 30}</span>% to start, &ge; <span id="threshold-high">${state.settings?.moistureHigh ?? 70}</span>% to stop`;
    } else {
      info.innerHTML = `Trigger range: moisture &lt; <span id="threshold-low">${state.settings?.moistureLow ?? 30}</span>% to turn ON, &ge; <span id="threshold-high">${state.settings?.moistureHigh ?? 70}</span>% to turn OFF`;
    }
  }

  const currentLabel = document.getElementById('chart-pump-cur');
  if (currentLabel) currentLabel.textContent = on ? '● ACTIVE' : '○ STANDBY';
}

// ----------------------------------------------------------
// Tank Level UI (Float Sensor)
// ----------------------------------------------------------
function updateTankUI(tankFull) {
  const valueEl  = document.getElementById('tank-value');
  const statusEl = document.getElementById('tank-status');
  const barEl    = document.getElementById('tank-bar');
  const cardEl   = document.getElementById('card-tank');

  if (!valueEl || !statusEl || !barEl) return;

  if (tankFull) {
    valueEl.textContent  = '100%';
    valueEl.style.color  = 'var(--cyan-accent)';
    statusEl.innerHTML   = '<span style="color:var(--green-accent)">● NORMAL</span> &middot; Water present';
    barEl.style.height   = '100%';
    if (cardEl) cardEl.style.borderColor = '';
  } else {
    valueEl.textContent  = '0%';
    valueEl.style.color  = 'var(--rose-accent)';
    statusEl.innerHTML   = '<span style="color:var(--rose-accent)">● EMPTY</span> &middot; Refill required';
    barEl.style.height   = '4%';
    if (cardEl) cardEl.style.borderColor = 'rgba(244,63,94,0.25)';
  }
}

// ----------------------------------------------------------
// SD Card Status Badge
// ----------------------------------------------------------
function updateSDStatus(sdAvailable, sdLogging) {
  const badge  = document.getElementById('sd-badge');
  const txtEl  = document.getElementById('sd-status-text');

  if (!sdAvailable) {
    badge.style.borderColor = 'rgba(100,100,120,0.35)';
    badge.style.color       = 'rgba(200,200,220,0.4)';
    txtEl.textContent       = 'SD Offline';
    badge.title             = 'SD card not detected — insert card to enable logging';
  } else if (sdLogging) {
    badge.style.borderColor = 'rgba(167,139,250,0.5)';
    badge.style.color       = '#a78bfa';
    txtEl.textContent       = 'SD Logging';
    badge.title             = 'SD card active — writing CSV data every 30s';
  } else {
    badge.style.borderColor = 'rgba(167,139,250,0.3)';
    badge.style.color       = '#c4b5fd';
    txtEl.textContent       = 'SD Ready';
    badge.title             = 'SD card mounted — first log pending';
  }
}

// ----------------------------------------------------------
// Mode UI
// ----------------------------------------------------------
function updateModeUI(auto) {
  const btnAuto   = document.getElementById('btn-auto');
  const btnManual = document.getElementById('btn-manual');

  if (btnAuto)   { btnAuto.className   = 'mode-btn' + (auto ? ' active' : '');  btnAuto.setAttribute('aria-pressed', auto); }
  if (btnManual) { btnManual.className = 'mode-btn' + (!auto ? ' active' : ''); btnManual.setAttribute('aria-pressed', !auto); }

  state.autoMode = auto;

  // Re-draw pump UI immediately so button states and status text reflect new mode
  updatePumpUI(state.pumpState);
}

// ----------------------------------------------------------
// Alerts
// ----------------------------------------------------------
function updateAlerts(alerts, count) {
  const list   = document.getElementById('alerts-list');
  const badge  = document.getElementById('alert-count');
  const flashContainer = document.getElementById('flash-alerts-container');

  badge.textContent = count;
  badge.className   = 'alert-count-badge' + (count === 0 ? ' none' : '');

  if (count === 0) {
    list.innerHTML = `
      <div class="no-alerts">
        <span>✅</span>
        <span>All farm conditions are within normal parameters.</span>
      </div>`;
    if (flashContainer) flashContainer.innerHTML = '';
    return;
  }

  const icons = {
    DROUGHT: '🏜️',
    HEAT:    '🔥',
    FROST:   '❄️',
    LOW:     '💧',
    DISEASE: '🍄',
    PUMP:    '⚙️',
    TANK:    '🪣',
  };

  const listHtml = alerts.map(msg => {
    const icon = Object.keys(icons).find(k => msg.toUpperCase().includes(k));
    return `
      <div class="alert-item" role="alert">
        <span class="alert-item-icon">${icons[icon] || '⚠️'}</span>
        <span>${msg}</span>
      </div>`;
  }).join('');
  list.innerHTML = listHtml;

  // Render high-visibility flash cards at the top of the main area (avoid scroll)
  if (flashContainer) {
    flashContainer.innerHTML = alerts.map(msg => {
      const iconKey = Object.keys(icons).find(k => msg.toUpperCase().includes(k)) || '⚠️';
      const isDanger = ['DROUGHT', 'HEAT', 'FROST', 'TANK'].some(k => msg.toUpperCase().includes(k));
      const severity = isDanger ? 'danger' : 'warning';
      const title = isDanger ? 'Critical Alert' : 'System Warning';
      const icon = icons[iconKey] || '⚠️';
      
      return `
        <div class="flash-alert-card ${severity}" role="alert">
          <div class="flash-alert-icon">${icon}</div>
          <div class="flash-alert-content">
            <div class="flash-alert-title">${title}</div>
            <div class="flash-alert-message">${msg}</div>
          </div>
        </div>`;
    }).join('');
  }
}

// ----------------------------------------------------------
// Charts (Chart.js)
// ----------------------------------------------------------
let charts = {};

const CHART_DEFAULTS = {
  responsive:          true,
  maintainAspectRatio: false,
  animation:           { duration: 500, easing: 'easeInOutQuart' },
  plugins: {
    legend: { display: false },
    tooltip: {
      backgroundColor: 'rgba(10,26,14,0.9)',
      borderColor: 'rgba(34,197,94,0.3)',
      borderWidth: 1,
      titleColor: '#f0fdf4',
      bodyColor: 'rgba(240,253,244,0.7)',
      padding: 10,
      cornerRadius: 8,
    },
  },
  scales: {
    x: {
      grid:  { color: 'rgba(34,197,94,0.05)', drawBorder: false },
      ticks: { color: 'rgba(240,253,244,0.3)', font: { size: 9 }, maxTicksLimit: 6 },
    },
    y: {
      grid:  { color: 'rgba(34,197,94,0.07)', drawBorder: false },
      ticks: { color: 'rgba(240,253,244,0.4)', font: { size: 10 } },
    },
  },
};

function makeGradient(ctx, colorTop, colorBot) {
  const g = ctx.createLinearGradient(0, 0, 0, 220);
  g.addColorStop(0,   colorTop);
  g.addColorStop(1,   colorBot);
  return g;
}

function initCharts() {
  const ctxSoil  = document.getElementById('chart-soil').getContext('2d');
  const ctxTemp  = document.getElementById('chart-temp').getContext('2d');
  const ctxHumid = document.getElementById('chart-humid').getContext('2d');
  const ctxPump  = document.getElementById('chart-pump').getContext('2d');

  const emptyLabels = Array(CONFIG.MAX_CHART).fill('');

  // Soil Moisture Chart
  charts.soil = new Chart(ctxSoil, {
    type: 'line',
    data: {
      labels: [...emptyLabels],
      datasets: [{
        data: Array(CONFIG.MAX_CHART).fill(null),
        borderColor: '#fbbf24',
        backgroundColor: makeGradient(ctxSoil, 'rgba(251,191,36,0.25)', 'rgba(251,191,36,0.02)'),
        borderWidth: 2.5,
        fill: true,
        tension: 0.4,
        pointRadius: 3,
        pointBackgroundColor: '#fbbf24',
        pointHoverRadius: 6,
        spanGaps: true,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
      scales: {
        ...CHART_DEFAULTS.scales,
        y: { ...CHART_DEFAULTS.scales.y, min: 0, max: 100,
             ticks: { ...CHART_DEFAULTS.scales.y.ticks, callback: v => v + '%' } },
      },
    },
  });

  // Temperature Chart
  charts.temp = new Chart(ctxTemp, {
    type: 'line',
    data: {
      labels: [...emptyLabels],
      datasets: [{
        data: Array(CONFIG.MAX_CHART).fill(null),
        borderColor: '#f43f5e',
        backgroundColor: makeGradient(ctxTemp, 'rgba(244,63,94,0.25)', 'rgba(244,63,94,0.02)'),
        borderWidth: 2.5,
        fill: true,
        tension: 0.4,
        pointRadius: 3,
        pointBackgroundColor: '#f43f5e',
        pointHoverRadius: 6,
        spanGaps: true,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
      scales: {
        ...CHART_DEFAULTS.scales,
        y: { ...CHART_DEFAULTS.scales.y,
             ticks: { ...CHART_DEFAULTS.scales.y.ticks, callback: v => v + '°C' } },
      },
    },
  });

  // Humidity Chart
  charts.humid = new Chart(ctxHumid, {
    type: 'line',
    data: {
      labels: [...emptyLabels],
      datasets: [{
        data: Array(CONFIG.MAX_CHART).fill(null),
        borderColor: '#60a5fa',
        backgroundColor: makeGradient(ctxHumid, 'rgba(96,165,250,0.25)', 'rgba(96,165,250,0.02)'),
        borderWidth: 2.5,
        fill: true,
        tension: 0.4,
        pointRadius: 3,
        pointBackgroundColor: '#60a5fa',
        pointHoverRadius: 6,
        spanGaps: true,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
      scales: {
        ...CHART_DEFAULTS.scales,
        y: { ...CHART_DEFAULTS.scales.y, min: 0, max: 100,
             ticks: { ...CHART_DEFAULTS.scales.y.ticks, callback: v => v + '%' } },
      },
    },
  });

  // Pump Activity (bar chart — 0 or 1)
  charts.pump = new Chart(ctxPump, {
    type: 'bar',
    data: {
      labels: [...emptyLabels],
      datasets: [{
        data: Array(CONFIG.MAX_CHART).fill(null),
        backgroundColor: (ctx) => {
          const v = ctx.raw;
          return v === 1 ? 'rgba(34,197,94,0.6)' : 'rgba(244,63,94,0.3)';
        },
        borderColor: (ctx) => {
          const v = ctx.raw;
          return v === 1 ? '#22c55e' : '#f43f5e';
        },
        borderWidth: 1.5,
        borderRadius: 4,
      }],
    },
    options: {
      ...CHART_DEFAULTS,
      scales: {
        ...CHART_DEFAULTS.scales,
        y: { ...CHART_DEFAULTS.scales.y, min: 0, max: 1, stepSize: 1,
             ticks: { ...CHART_DEFAULTS.scales.y.ticks,
                      callback: v => v === 1 ? 'ON' : v === 0 ? 'OFF' : '' } },
      },
    },
  });
}

function pushChartData(label, soil, temp, humid, pump) {
  const d = state.data;
  const MAX = CONFIG.MAX_CHART;

  d.labels.push(label);
  d.soil.push(soil);
  d.temp.push(temp);
  d.humid.push(humid);
  d.pump.push(pump ? 1 : 0);

  if (d.labels.length > MAX) {
    d.labels.shift();
    d.soil.shift();
    d.temp.shift();
    d.humid.shift();
    d.pump.shift();
  }

  charts.soil.data.labels  = [...d.labels];
  charts.temp.data.labels  = [...d.labels];
  charts.humid.data.labels = [...d.labels];
  charts.pump.data.labels  = [...d.labels];

  charts.soil.data.datasets[0].data  = [...d.soil];
  charts.temp.data.datasets[0].data  = [...d.temp];
  charts.humid.data.datasets[0].data = [...d.humid];
  charts.pump.data.datasets[0].data  = [...d.pump];

  charts.soil.update('none');
  charts.temp.update('none');
  charts.humid.update('none');
  charts.pump.update();

  // Update current value displays
  document.getElementById('chart-soil-cur').textContent  = soil + ' %';
  document.getElementById('chart-temp-cur').textContent  = temp.toFixed(1) + ' °C';
  document.getElementById('chart-humid-cur').textContent = humid.toFixed(1) + ' %';
}

function updateCharts(data) {
  pushChartData(
    data.timestamp || getLocalTime(),
    data.soilMoisture,
    data.temperature,
    data.humidity,
    data.pumpState,
  );
}

// ----------------------------------------------------------
// Data Log
// ----------------------------------------------------------
function appendLog(data) {
  const tbody = document.getElementById('log-tbody');

  // Remove placeholder row on first real data
  if (tbody.querySelector('[colspan]')) tbody.innerHTML = '';

  const row = document.createElement('tr');
  row.innerHTML = `
    <td>${data.timestamp || '--'}</td>
    <td>${data.datestamp || '--'}</td>
    <td class="td-soil">${data.soilMoisture}%</td>
    <td class="td-temp">${data.temperature.toFixed(1)}°C</td>
    <td class="td-humid">${data.humidity.toFixed(1)}%</td>
    <td class="${data.tankFull ? 'td-pump on' : 'td-pump off'}">${data.tankFull ? '🟢 Full' : '🔴 Empty'}</td>
    <td class="td-pump ${data.pumpState ? 'on' : 'off'}">${data.pumpState ? '● ON' : '○ OFF'}</td>
    <td>${data.autoMode ? '⚡ Auto' : '🖐 Manual'}</td>
    <td style="color: ${data.sdAvailable ? (data.sdLogging ? '#a78bfa' : '#c4b5fd') : 'rgba(200,200,220,0.35)'};">
      ${data.sdAvailable ? (data.sdLogging ? '💾 Saved' : '💾 Ready') : '— Offline'}
    </td>
  `;

  tbody.insertBefore(row, tbody.firstChild);

  // Trim log
  while (tbody.rows.length > CONFIG.MAX_LOG) {
    tbody.deleteRow(tbody.rows.length - 1);
  }

  state.logRows.unshift(data);
  if (state.logRows.length > CONFIG.MAX_LOG) state.logRows.pop();
}

function clearLog() {
  document.getElementById('log-tbody').innerHTML = `
    <tr>
      <td colspan="7" style="text-align:center; color: var(--text-muted); padding: 1.5rem;">
        Log cleared.
      </td>
    </tr>`;
  state.logRows = [];
}

// ----------------------------------------------------------
// Pump & Mode Control (called from HTML buttons)
// ----------------------------------------------------------
function sendPumpOn() {
  // Always ensure manual mode is set on the ESP32 before turning pump on
  sendCommand({ action: 'set_auto', value: false });
  // Manual press always sends force:true so the firmware allows it
  // even when the tank guard is enabled and the tank reads empty.
  sendCommand({ action: 'pump_on', force: true });
  // Optimistic UI update
  updatePumpUI(true);
  updateModeUI(false);
  logPumpEventToCloud('manual_on', true);
}

function sendPumpOff() {
  sendCommand({ action: 'pump_off' });
  updatePumpUI(false);
  logPumpEventToCloud('manual_off', false);
}

function setMode(auto) {
  // When switching to manual, stop any auto-running pump first
  if (!auto && state.pumpState) {
    sendCommand({ action: 'pump_off' });
    updatePumpUI(false);
  }
  sendCommand({ action: 'set_auto', value: auto });
  updateModeUI(auto);
}

// ----------------------------------------------------------
// Header Clock (local time display)
// ----------------------------------------------------------
function startHeaderClock() {
  function tick() {
    const now  = new Date();
    const h    = String(now.getHours()).padStart(2, '0');
    const m    = String(now.getMinutes()).padStart(2, '0');
    const s    = String(now.getSeconds()).padStart(2, '0');
    document.getElementById('header-clock').textContent = `${h}:${m}:${s}`;
  }
  tick();
  setInterval(tick, 1000);
}

function getLocalTime() {
  const now = new Date();
  return `${String(now.getHours()).padStart(2,'0')}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`;
}

// ----------------------------------------------------------
// Demo Mode (simulated data when ESP32 not connected)
// ----------------------------------------------------------
let demoSoil    = 45;
let demoTemp    = 24.5;
let demoHumid   = 62.0;
let demoSeconds = 0;

function generateDemoData() {
  demoSeconds += 5;

  // Simulate soil drying out slowly unless pump is on
  if (state.pumpState) {
    demoSoil = Math.min(85, demoSoil + 3.5);
  } else {
    demoSoil = Math.max(5, demoSoil - 1.2);
  }

  // Tank level switch simulation
  const simTankFull = demoSoil > 15;

  // Tank Level Guard safety logic in demo
  if (SETTINGS.tankGuard && !simTankFull && state.pumpState) {
    state.pumpState = false;
    updatePumpUI(false);
    console.log('[DEMO] Pump emergency shutoff: tank empty (guard active)');
  }

  // Auto-irrigation logic in demo (uses dynamic settings thresholds)
  if (state.autoMode) {
    if (!state.pumpState && demoSoil < SETTINGS.moistureLow && (!SETTINGS.tankGuard || simTankFull)) {
      state.pumpState = true;
      updatePumpUI(true);
    } else if (state.pumpState && demoSoil >= SETTINGS.moistureHigh) {
      state.pumpState = false;
      updatePumpUI(false);
    }
  }

  demoTemp  += (Math.random() - 0.5) * 0.8;
  demoHumid += (Math.random() - 0.5) * 1.5;
  demoTemp   = Math.max(10, Math.min(45, demoTemp));
  demoHumid  = Math.max(15, Math.min(95, demoHumid));

  const now     = new Date();
  const alerts  = [];
  let alertCount = 0;

  // Alert generation (uses dynamic settings thresholds)
  if (demoSoil < SETTINGS.moistureLow) { 
    alerts.push('DROUGHT RISK: Soil moisture critically low (' + Math.round(demoSoil) + '%)'); 
    alertCount++; 
  }
  if (demoTemp > SETTINGS.tempHeat) { 
    alerts.push('HEAT STRESS: Temperature ' + demoTemp.toFixed(1) + '°C exceeds safe limit'); 
    alertCount++; 
  }
  if (demoTemp < SETTINGS.tempFrost) { 
    alerts.push('FROST WARNING: Temperature ' + demoTemp.toFixed(1) + '°C near freezing'); 
    alertCount++; 
  }
  if (demoHumid < SETTINGS.humidLow) { 
    alerts.push('LOW HUMIDITY: ' + demoHumid.toFixed(1) + '% — increased evaporation rate'); 
    alertCount++; 
  }
  if (demoHumid > SETTINGS.humidHigh) { 
    alerts.push('DISEASE RISK: Humidity ' + demoHumid.toFixed(1) + '% — fungal risk elevated'); 
    alertCount++; 
  }
  if (!simTankFull) {
    alerts.push('TANK EMPTY: Water reservoir low — refill required');
    alertCount++;
  }

  const demoPayload = {
    temperature:  parseFloat(demoTemp.toFixed(1)),
    humidity:     parseFloat(demoHumid.toFixed(1)),
    soilMoisture: Math.round(demoSoil),
    soilRaw:      Math.round(1000 + (100 - demoSoil) * 28),
    tankFull:     demoSoil > 15,   // Simulate tank emptying as soil dries very low
    pumpState:    state.pumpState,
    autoMode:     state.autoMode,
    timestamp:    `${String(now.getHours()).padStart(2,'0')}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`,
    datestamp:    `${now.getFullYear()}-${String(now.getMonth()+1).padStart(2,'0')}-${String(now.getDate()).padStart(2,'0')}`,
    alerts,
    alertCount,
    sdAvailable:  false,    // Demo: no SD card simulated
    sdLogging:    false,
  };

  processData(demoPayload);
}

function startDemo() {
  if (state.demoInterval) return;
  console.log('[DEMO] Starting simulated data feed');
  
  // Set connection badge to demo status cleanly without overwriting DOM structure
  updateConnectionBadge('demo');

  // Immediately generate first data point
  generateDemoData();
  state.demoInterval = setInterval(generateDemoData, 5000);
}

function stopDemo() {
  if (state.demoInterval) {
    clearInterval(state.demoInterval);
    state.demoInterval = null;
    console.log('[DEMO] Stopped — connected to ESP32');
  }
}

// ----------------------------------------------------------
// Helper: animate number value change
// ----------------------------------------------------------
function animateValue(elementId, value) {
  const el = document.getElementById(elementId);
  if (!el) return;
  el.textContent = value;
  animateElement(elementId);
}

function animateElement(elementId) {
  const el = document.getElementById(elementId);
  if (!el) return;
  el.classList.remove('value-update');
  void el.offsetWidth; // Trigger reflow
  el.classList.add('value-update');
}

// ----------------------------------------------------------
// Initialize
// ----------------------------------------------------------
document.addEventListener('DOMContentLoaded', () => {
  console.log('🌱 SmartFarm IoT Dashboard initializing…');
  startHeaderClock();
  initCharts();
  loadSettings();     // ← Load saved settings before connecting
  initMQTT();         // ← Connect to remote MQTT cloud broker for cross-network connectivity

  // Attempt WebSocket connection to ESP32 only if on HTTP (local development)
  if (window.location.protocol === 'http:' && CONFIG.ESP32_IP && CONFIG.ESP32_IP !== '192.168.1.100') {
    connectWebSocket();
  } else {
    updateConnectionBadge('connecting', 'Connecting to Cloud MQTT…');
    if (CONFIG.DEMO_MODE) startDemo();
  }
});

// Expose control functions globally for HTML onclick handlers
window.sendPumpOn  = sendPumpOn;
window.sendPumpOff = sendPumpOff;
window.setMode     = setMode;
window.clearLog    = clearLog;

// ============================================================
//  SETTINGS MODULE
//  KPI thresholds, pump safety, intervals, connection config
// ============================================================

// --- Default KPI values (mirrors config.h) ---
const SETTINGS_DEFAULTS = {
  moistureLow:     30,    // % — pump ON below this
  moistureHigh:    70,    // % — pump OFF above this
  tempHeat:        35,    // °C — heat stress alert
  tempFrost:        4,    // °C — frost warning
  humidLow:        30,    // % — low humidity alert
  humidHigh:       90,    // % — high humidity alert
  pumpMax:          5,    // minutes — max pump run time
  pumpCool:         1,    // minutes — cooldown period
  sensorInterval:   5,    // seconds — sensor read rate
  sdInterval:      30,    // seconds — SD card log rate
  maxLog:         100,    // rows — session log table
  tankGuard:     true,    // true = shutoff pump if tank empty (float sensor)
  esp32Ip:   CONFIG.ESP32_IP,
  wsPort:    CONFIG.WS_PORT,
  demoMode:  CONFIG.DEMO_MODE,
  autoReconnect: true,
};

// Active settings object (runtime copy)
let SETTINGS = { ...SETTINGS_DEFAULTS };

// --- Load settings from localStorage ---
function loadSettings() {
  try {
    const saved = localStorage.getItem('smartfarm_settings');
    if (saved) {
      const parsed = JSON.parse(saved);
      SETTINGS = { ...SETTINGS_DEFAULTS, ...parsed };
      console.log('[SETTINGS] Loaded from localStorage');
    }
  } catch (e) {
    console.warn('[SETTINGS] Failed to load from localStorage:', e);
    SETTINGS = { ...SETTINGS_DEFAULTS };
  }
  // Apply loaded settings to UI sliders & inputs
  applySettingsToUI();
  // Apply to runtime logic
  applySettingsToRuntime();
}

// --- Persist settings to localStorage ---
function persistSettings() {
  try {
    localStorage.setItem('smartfarm_settings', JSON.stringify(SETTINGS));
  } catch (e) {
    console.warn('[SETTINGS] Failed to save to localStorage:', e);
  }
}

// --- Populate UI controls from SETTINGS object ---
function applySettingsToUI() {
  // Sliders
  const sliderMap = {
    'moisture-low':     { val: SETTINGS.moistureLow,    suffix: '%'    },
    'moisture-high':    { val: SETTINGS.moistureHigh,   suffix: '%'    },
    'temp-heat':        { val: SETTINGS.tempHeat,       suffix: '°C'   },
    'temp-frost':       { val: SETTINGS.tempFrost,      suffix: '°C'   },
    'humid-low':        { val: SETTINGS.humidLow,       suffix: '%'    },
    'humid-high':       { val: SETTINGS.humidHigh,      suffix: '%'    },
    'pump-max':         { val: SETTINGS.pumpMax,        suffix: ' min' },
    'pump-cool':        { val: SETTINGS.pumpCool,       suffix: ' min' },
    'sensor-interval':  { val: SETTINGS.sensorInterval, suffix: ' s'   },
    'sd-interval':      { val: SETTINGS.sdInterval,     suffix: ' s'   },
    'max-log':          { val: SETTINGS.maxLog,         suffix: ''     },
  };

  Object.entries(sliderMap).forEach(([key, { val, suffix }]) => {
    const slider = document.getElementById(`sl-${key}`);
    const badge  = document.getElementById(`val-${key}`);
    if (slider) slider.value = val;
    if (badge)  badge.textContent = val + suffix;
  });

  // Text inputs
  const ipEl   = document.getElementById('input-esp32-ip');
  const portEl = document.getElementById('input-ws-port');
  if (ipEl)   ipEl.value   = SETTINGS.esp32Ip;
  if (portEl) portEl.value = SETTINGS.wsPort;

  // Toggles
  const demoEl      = document.getElementById('toggle-demo');
  const reconnectEl = document.getElementById('toggle-reconnect');
  const guardEl     = document.getElementById('toggle-tank-guard');
  if (demoEl)      demoEl.checked      = SETTINGS.demoMode;
  if (reconnectEl) reconnectEl.checked = SETTINGS.autoReconnect;
  if (guardEl)     guardEl.checked     = SETTINGS.tankGuard;

  // Update threshold visualiser
  updateThresholdVisualiser();

  // Update about tab WS URL
  const aboutWs = document.getElementById('about-ws-url');
  if (aboutWs) aboutWs.textContent = `ws://${SETTINGS.esp32Ip}:${SETTINGS.wsPort}`;
}

// --- Apply settings to in-memory runtime (demo mode alert logic, CONFIG) ---
function applySettingsToRuntime() {
  CONFIG.DEMO_MODE     = SETTINGS.demoMode;
  CONFIG.ESP32_IP      = SETTINGS.esp32Ip;
  CONFIG.WS_PORT       = SETTINGS.wsPort;
  CONFIG.MAX_LOG       = SETTINGS.maxLog;
}

// --- Send threshold settings to ESP32 via WebSocket ---
function sendSettingsToESP32() {
  const cmd = {
    action:           'set_thresholds',
    moisture_low:     SETTINGS.moistureLow,
    moisture_high:    SETTINGS.moistureHigh,
    temp_heat:        SETTINGS.tempHeat,
    temp_frost:       SETTINGS.tempFrost,
    humid_low:        SETTINGS.humidLow,
    humid_high:       SETTINGS.humidHigh,
    pump_max_min:     SETTINGS.pumpMax,
    pump_cool_min:    SETTINGS.pumpCool,
    sensor_interval_s: SETTINGS.sensorInterval,
    sd_interval_s:    SETTINGS.sdInterval,
    tank_guard:       SETTINGS.tankGuard,
  };
  sendCommand(cmd);
  console.log('[SETTINGS] Sent thresholds to ESP32:', cmd);
}

// --- Live slider handler (fires on input) ---
function onSlider(key, rawValue, suffix) {
  const val = parseFloat(rawValue);
  const badge = document.getElementById(`val-${key}`);
  if (badge) badge.textContent = val + suffix;

  // Update SETTINGS in real-time for immediate feedback
  const keyMap = {
    'moisture-low':    'moistureLow',
    'moisture-high':   'moistureHigh',
    'temp-heat':       'tempHeat',
    'temp-frost':      'tempFrost',
    'humid-low':       'humidLow',
    'humid-high':      'humidHigh',
    'pump-max':        'pumpMax',
    'pump-cool':       'pumpCool',
    'sensor-interval': 'sensorInterval',
    'sd-interval':     'sdInterval',
    'max-log':         'maxLog',
  };
  if (keyMap[key]) SETTINGS[keyMap[key]] = val;

  // Update visualiser if moisture thresholds changed
  if (key === 'moisture-low' || key === 'moisture-high') {
    updateThresholdVisualiser();
  }

  // Validate moisture constraint: low must be < high
  if (key === 'moisture-low' && val >= SETTINGS.moistureHigh) {
    const highSlider = document.getElementById('sl-moisture-high');
    const newHigh = Math.min(100, val + 5);
    if (highSlider) { highSlider.value = newHigh; }
    const highBadge = document.getElementById('val-moisture-high');
    if (highBadge) highBadge.textContent = newHigh + '%';
    SETTINGS.moistureHigh = newHigh;
    updateThresholdVisualiser();
  }
}

// --- Toggle handler ---
function onToggle(key, checked) {
  const keyMap = { 
    demo: 'demoMode', 
    reconnect: 'autoReconnect',
    'tank-guard': 'tankGuard'
  };
  if (keyMap[key]) SETTINGS[keyMap[key]] = checked;
}

// --- Threshold visualiser: shows dry/optimal/wet zones ---
function updateThresholdVisualiser() {
  const low  = SETTINGS.moistureLow;
  const high = SETTINGS.moistureHigh;

  const dryEl  = document.getElementById('viz-dry');
  const goodEl = document.getElementById('viz-good');
  const wetEl  = document.getElementById('viz-wet');

  if (!dryEl || !goodEl || !wetEl) return;

  const goodWidth = high - low;
  const wetWidth  = 100 - high;

  dryEl.style.width  = low + '%';
  goodEl.style.left  = low + '%';
  goodEl.style.width = goodWidth + '%';
  wetEl.style.width  = wetWidth + '%';

  // Hide labels if zone is too narrow
  dryEl.style.fontSize  = low < 12     ? '0' : '';
  goodEl.style.fontSize = goodWidth < 15 ? '0' : '';
  wetEl.style.fontSize  = wetWidth < 12  ? '0' : '';
}

// --- Save & Apply (called by Save button) ---
function saveAndApplySettings() {
  // --- 1. Detect if IP or Port changed ---
  const ipEl   = document.getElementById('input-esp32-ip');
  const portEl = document.getElementById('input-ws-port');
  
  const targetIp   = ipEl ? ipEl.value.trim() : SETTINGS.esp32Ip;
  const targetPort = portEl ? (parseInt(portEl.value) || 81) : SETTINGS.wsPort;

  const ipOrPortChanged = (targetIp !== SETTINGS.esp32Ip || targetPort !== SETTINGS.wsPort);

  // --- 2. Update SETTINGS object ---
  SETTINGS.esp32Ip = targetIp;
  SETTINGS.wsPort  = targetPort;

  // --- 3. Persist to localStorage ---
  persistSettings();

  // --- 4. Apply to runtime CONFIG ---
  applySettingsToRuntime();

  // --- 5. Update pump threshold display on main dashboard ---
  const thLow  = document.getElementById('threshold-low');
  const thHigh = document.getElementById('threshold-high');
  if (thLow)  thLow.textContent  = SETTINGS.moistureLow;
  if (thHigh) thHigh.textContent = SETTINGS.moistureHigh;

  // --- 6. Connection management ---
  if (ipOrPortChanged) {
    // IP/Port changed: Close old connection and establish new one
    console.log('[SETTINGS] IP or Port changed. Reconnecting...');
    stopDemo();
    if (state.ws) {
      state.ws.onclose = null;
      state.ws.onerror = null;
      state.ws.close();
      state.ws        = null;
      state.connected = false;
    }
    clearReconnectTimer();

    if (SETTINGS.esp32Ip && SETTINGS.esp32Ip !== '192.168.1.100') {
      updateConnectionBadge('connecting');
      connectWebSocket();
    } else if (SETTINGS.demoMode) {
      startDemo();
    } else {
      updateConnectionBadge('disconnected');
    }
  } else {
    // IP/Port did NOT change
    if (state.connected) {
      // Already connected: transmit the settings to the ESP32 directly!
      sendSettingsToESP32();
    } else {
      // Not connected currently: check if demo mode toggle changed
      if (SETTINGS.demoMode) {
        startDemo();
      } else {
        stopDemo();
        // If not demo mode and not connected, trigger connection retry if we have an IP
        if (SETTINGS.esp32Ip && SETTINGS.esp32Ip !== '192.168.1.100' && !state.reconnectTimer) {
          connectWebSocket();
        } else {
          updateConnectionBadge('disconnected');
        }
      }
    }
  }

  // --- 7. Close drawer & show confirmation ---
  closeSettings();
  showToast('✅ Settings saved & applied!');
  console.log('[SETTINGS] Saved:', SETTINGS);
}

// --- Reset to factory defaults ---
function resetSettingsToDefaults() {
  SETTINGS = { ...SETTINGS_DEFAULTS };
  applySettingsToUI();
  showToast('↺ Settings reset to defaults');
}

// --- Toast notification ---
let toastTimer = null;
function showToast(msg) {
  const toast = document.getElementById('save-toast');
  if (!toast) return;
  toast.textContent = msg;
  toast.classList.add('show');
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('show'), 3000);
}

// --- Drawer open/close ---
function openSettings() {
  const drawer  = document.getElementById('settings-drawer');
  const overlay = document.getElementById('settings-overlay');
  const btn     = document.getElementById('open-settings-btn');

  drawer?.classList.add('open');
  overlay?.classList.add('open');
  btn?.setAttribute('aria-expanded', 'true');
  document.body.style.overflow = 'hidden';

  // Sync about tab connection status
  updateAboutConnectionStatus();
}

function closeSettings() {
  const drawer  = document.getElementById('settings-drawer');
  const overlay = document.getElementById('settings-overlay');
  const btn     = document.getElementById('open-settings-btn');

  drawer?.classList.remove('open');
  overlay?.classList.remove('open');
  btn?.setAttribute('aria-expanded', 'false');
  document.body.style.overflow = '';
}

// --- Tab switcher ---
function switchTab(tabId) {
  // Deactivate all tabs and panels
  document.querySelectorAll('.settings-tab').forEach(t => {
    t.classList.remove('active');
    t.setAttribute('aria-selected', 'false');
  });
  document.querySelectorAll('.settings-tab-panel').forEach(p => {
    p.classList.remove('active');
  });

  // Activate selected tab and panel
  const tab   = document.getElementById(`tab-${tabId}`);
  const panel = document.getElementById(`panel-${tabId}`);
  tab?.classList.add('active');
  tab?.setAttribute('aria-selected', 'true');
  panel?.classList.add('active');
}

// --- Sync About tab connection status ---
function updateAboutConnectionStatus() {
  const statusBox = document.getElementById('about-conn-status');
  const statusTxt = document.getElementById('about-conn-text');
  const wsUrl     = document.getElementById('about-ws-url');

  if (wsUrl) wsUrl.textContent = `ws://${SETTINGS.esp32Ip}:${SETTINGS.wsPort}`;

  if (!statusBox || !statusTxt) return;

  if (state.connected) {
    statusBox.className = 'settings-info success';
    statusTxt.textContent = `Connected to ESP32 at ${SETTINGS.esp32Ip}`;
  } else if (state.demoInterval) {
    statusBox.className = 'settings-info warn';
    statusTxt.textContent = 'Demo Mode — not connected to real hardware';
  } else {
    statusBox.className = 'settings-info info';
    statusTxt.textContent = `Disconnected — retrying ${SETTINGS.esp32Ip}:${SETTINGS.wsPort}`;
  }
}

// Keyboard: close drawer on Escape
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') closeSettings();
});

// Expose settings functions globally for HTML onclick handlers
window.openSettings          = openSettings;
window.closeSettings         = closeSettings;
window.switchTab             = switchTab;
window.onSlider              = onSlider;
window.onToggle              = onToggle;
window.saveAndApplySettings  = saveAndApplySettings;
window.resetSettingsToDefaults = resetSettingsToDefaults;
window.logoutSupabase        = logoutSupabase;

