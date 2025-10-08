// app.js (usar <script type="module"> en HTML)
const brokerHost = "18.218.20.155";
const brokerPort = 9001;
const topicMonitor = "esp32/led";
const topicConfig  = "esp32/config";
const username = "esp32";
const password = "13310625"; // contraseña provista

const KNOWN_COLOR_CLASSES = {
  red: "color-red",
  green: "color-green",
  blue: "color-blue"
};

let client = null;
let messageCount = 0;

const el = {
  circle: document.getElementById("circle"),
  status: document.getElementById("status"),
  colorText: document.getElementById("colorText"),
  messageCount: document.getElementById("messageCount"),
  interval: document.getElementById("interval"),
  colors: document.getElementById("colors"),
  configStatus: document.getElementById("config-status"),
  sendConfigBtn: document.getElementById("sendConfigBtn")
};

function init() {
  // Event listeners
  el.sendConfigBtn.addEventListener("click", sendConfig);

  // Crear cliente Paho
  client = new Paho.MQTT.Client(brokerHost, brokerPort, "webclient_" + Math.random().toString(16).slice(2,10));
  client.onConnectionLost = onConnectionLost;
  client.onMessageArrived = onMessageArrived;

  connectBroker();
}

function connectBroker() {
  const opts = {
    userName: username,
    password: password,
    timeout: 10,
    keepAliveInterval: 30,
    onSuccess: onConnected,
    onFailure: (err) => {
      console.error("Conexión fallida:", err);
      setStatus(`Fallo la conexión MQTT: ${err.errorMessage || "error"}`, "disconnected");
    }
  };

  console.info(`Intentando conectar a ${brokerHost}:${brokerPort}`);
  try {
    client.connect(opts);
  } catch (e) {
    console.error("Error al iniciar conexión MQTT:", e);
    setStatus("Error al iniciar conexión MQTT", "disconnected");
  }
}

function onConnected() {
  console.info("Conectado al broker MQTT");
  setStatus("Conectado a MQTT", "connected");
  try {
    client.subscribe(topicMonitor);
    console.info("Suscrito a:", topicMonitor);
  } catch (e) {
    console.error("Error suscribiendo:", e);
  }
}

function onConnectionLost(response) {
  console.warn("Conexión perdida:", response && response.errorMessage);
  setStatus("Desconectado de MQTT", "disconnected");

  // Intento de reconexión simple (rápido)
  setTimeout(() => {
    setStatus("Reconectando...", "connecting");
    connectBroker();
  }, 2000);
}

function onMessageArrived(message) {
  const colorRaw = (message.payloadString || "").trim().toLowerCase();
  messageCount++;
  el.messageCount.textContent = messageCount;
  el.colorText.textContent = colorRaw || "---";

  updateCircleColor(colorRaw);
}

function updateCircleColor(color) {
  // Remover clases conocidas
  Object.values(KNOWN_COLOR_CLASSES).forEach(c => el.circle.classList.remove(c));
  el.circle.classList.remove("glow");

  if (color && KNOWN_COLOR_CLASSES[color]) {
    el.circle.classList.add(KNOWN_COLOR_CLASSES[color], "glow");
    // quitar glow después de un momento
    setTimeout(() => el.circle.classList.remove("glow"), 900);
  } else {
    // color desconocido -> restore base look
    el.circle.style.background = "";
  }
}

function sendConfig() {
  if (!client || typeof client.isConnected === "function" && !client.isConnected()) {
    el.configStatus.textContent = "Error: No conectado al broker.";
    return;
  }

  const intervalVal = parseInt(el.interval.value, 10);
  const colorsVal = el.colors.value.trim();

  if (isNaN(intervalVal) || intervalVal < 1) {
    el.configStatus.textContent = "Error: El intervalo debe ser un número positivo.";
    return;
  }

  const payload = {
    interval_sec: intervalVal,
    colors: colorsVal
  };

  const payloadStr = JSON.stringify(payload);
  const msg = new Paho.MQTT.Message(payloadStr);
  msg.destinationName = topicConfig;

  try {
    client.send(msg);
    el.configStatus.textContent = `Configuración enviada. Intervalo: ${intervalVal}s. Colores: ${colorsVal}`;
    console.log("Configuración enviada:", payloadStr);
  } catch (e) {
    console.error("Error al enviar configuración:", e);
    el.configStatus.textContent = "Error al enviar configuración.";
  }
}

// Helper para estados
function setStatus(text, cls) {
  el.status.textContent = text;
  el.status.className = cls || "";
}

// Inicializa al cargar el script (DOM ya cargado porque script está al final)
init();
