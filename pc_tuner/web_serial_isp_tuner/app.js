const PARAMS = [
  { key: "BRIGHTNESS", label: "Brightness", min: 0, max: 255, step: 1 },
  { key: "CONTRAST", label: "Contrast", min: 0, max: 255, step: 1 },
  { key: "SHARPNESS", label: "Sharpness", min: 0, max: 255, step: 1 },
  { key: "SATURATION", label: "Saturation", min: 0, max: 255, step: 1 },
  { key: "AE_COMP", label: "AE Compensation", min: 90, max: 250, step: 1 },
  { key: "DPC", label: "DPC Strength", min: 0, max: 255, step: 1 },
  { key: "DRC", label: "DRC Strength", min: 0, max: 255, step: 1 },
  { key: "AWB_CT", label: "AWB Color Temp", min: 1500, max: 12000, step: 10 },
];

const ui = {
  connectBtn: document.getElementById("connectBtn"),
  disconnectBtn: document.getElementById("disconnectBtn"),
  refreshBtn: document.getElementById("refreshBtn"),
  autoPreviewBtn: document.getElementById("autoPreviewBtn"),
  snapBtn: document.getElementById("snapBtn"),
  clearLogBtn: document.getElementById("clearLogBtn"),
  baudSelect: document.getElementById("baudSelect"),
  statusText: document.getElementById("statusText"),
  statusDot: document.getElementById("statusDot"),
  logBox: document.getElementById("logBox"),
  paramGrid: document.getElementById("paramGrid"),
  previewImage: document.getElementById("previewImage"),
};

const state = {
  port: null,
  reader: null,
  writer: null,
  textEncoder: new TextEncoder(),
  textDecoder: new TextDecoder(),
  connected: false,
  rxBuffer: new Uint8Array(0),
  binaryRemaining: 0,
  binaryChunks: [],
  autoPreviewTimer: null,
  previewBusy: false,
  previewUrl: null,
};

function concatUint8(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

function findNewline(buf) {
  for (let i = 0; i < buf.length; i += 1) {
    if (buf[i] === 0x0a) {
      return i;
    }
  }
  return -1;
}

function log(message) {
  const ts = new Date().toLocaleTimeString();
  ui.logBox.textContent += `[${ts}] ${message}\n`;
  ui.logBox.scrollTop = ui.logBox.scrollHeight;
}

function updateConnectionUi() {
  ui.statusText.textContent = state.connected ? "Connected" : "Disconnected";
  ui.statusDot.classList.toggle("connected", state.connected);
  ui.connectBtn.disabled = state.connected;
  ui.disconnectBtn.disabled = !state.connected;
  ui.refreshBtn.disabled = !state.connected;
  ui.snapBtn.disabled = !state.connected;
  ui.autoPreviewBtn.disabled = !state.connected;
}

function renderParams() {
  ui.paramGrid.innerHTML = "";
  for (const param of PARAMS) {
    const card = document.createElement("article");
    card.className = "param-card";
    card.innerHTML = `
      <header>
        <label for="slider-${param.key}">${param.label}</label>
        <span class="value" id="value-${param.key}">-</span>
      </header>
      <input id="slider-${param.key}" type="range" min="${param.min}" max="${param.max}" step="${param.step}" value="${param.min}">
      <div class="param-actions">
        <button id="apply-${param.key}" disabled>Apply</button>
        <button id="read-${param.key}" disabled>Read</button>
      </div>
    `;
    ui.paramGrid.appendChild(card);

    const slider = document.getElementById(`slider-${param.key}`);
    const value = document.getElementById(`value-${param.key}`);
    const applyBtn = document.getElementById(`apply-${param.key}`);
    const readBtn = document.getElementById(`read-${param.key}`);

    slider.addEventListener("input", () => {
      value.textContent = slider.value;
    });

    applyBtn.addEventListener("click", async () => {
      await sendCommand(`SET ${param.key} ${slider.value}`);
    });

    readBtn.addEventListener("click", async () => {
      await sendCommand(`GET ${param.key}`);
    });

    param.slider = slider;
    param.valueLabel = value;
    param.applyBtn = applyBtn;
    param.readBtn = readBtn;
  }
}

function updateParamUiEnabled() {
  for (const param of PARAMS) {
    param.applyBtn.disabled = !state.connected;
    param.readBtn.disabled = !state.connected;
    param.slider.disabled = !state.connected;
  }
}

async function connectSerial() {
  if (!("serial" in navigator)) {
    alert("This browser does not support Web Serial. Please use Edge or Chrome.");
    return;
  }

  state.port = await navigator.serial.requestPort();
  await state.port.open({ baudRate: Number(ui.baudSelect.value) });
  state.writer = state.port.writable.getWriter();
  state.reader = state.port.readable.getReader();
  state.connected = true;
  updateConnectionUi();
  updateParamUiEnabled();
  log("serial connected");
  readLoop().catch((err) => {
    log(`read loop ended: ${err.message}`);
  });
}

async function disconnectSerial() {
  stopAutoPreview();
  state.connected = false;
  updateConnectionUi();
  updateParamUiEnabled();

  try {
    if (state.reader) {
      await state.reader.cancel();
      state.reader.releaseLock();
      state.reader = null;
    }
  } catch (err) {
    log(`reader close warning: ${err.message}`);
  }

  try {
    if (state.writer) {
      state.writer.releaseLock();
      state.writer = null;
    }
  } catch (err) {
    log(`writer close warning: ${err.message}`);
  }

  try {
    if (state.port) {
      await state.port.close();
      state.port = null;
    }
  } catch (err) {
    log(`port close warning: ${err.message}`);
  }

  log("serial disconnected");
}

async function sendCommand(command) {
  if (!state.connected || !state.writer) {
    return;
  }
  log(`> ${command}`);
  await state.writer.write(state.textEncoder.encode(`${command}\n`));
}

function handleTextLine(line) {
  if (!line) {
    return;
  }

  log(`< ${line}`);

  if (line.startsWith("VAL ")) {
    const [, key, value] = line.split(/\s+/);
    const param = PARAMS.find((p) => p.key === key);
    if (param) {
      param.slider.value = value;
      param.valueLabel.textContent = value;
    }
    return;
  }

  if (line.startsWith("JPEG ")) {
    const [, lengthText] = line.split(/\s+/);
    state.binaryRemaining = Number(lengthText);
    state.binaryChunks = [];
    state.previewBusy = true;
    return;
  }
}

function appendBinaryChunk(chunk) {
  if (state.binaryRemaining <= 0) {
    return new Uint8Array(0);
  }

  let bytes = chunk;
  let remainder = new Uint8Array(0);
  if (bytes.length > state.binaryRemaining) {
    remainder = bytes.slice(state.binaryRemaining);
    bytes = bytes.slice(0, state.binaryRemaining);
  }
  state.binaryChunks.push(bytes);
  state.binaryRemaining -= bytes.length;

  if (state.binaryRemaining === 0) {
    const blob = new Blob(state.binaryChunks, { type: "image/jpeg" });
    if (state.previewUrl) {
      URL.revokeObjectURL(state.previewUrl);
    }
    state.previewUrl = URL.createObjectURL(blob);
    ui.previewImage.src = state.previewUrl;
    state.binaryChunks = [];
    state.previewBusy = false;
    log("< JPEG frame complete");
  }

  return remainder;
}

async function readLoop() {
  while (state.connected && state.reader) {
    let { value, done } = await state.reader.read();
    if (done) {
      break;
    }
    if (!value) {
      continue;
    }

    while (true) {
      if (state.binaryRemaining > 0) {
        if (value.length > 0) {
          value = appendBinaryChunk(value);
        }
        if (state.binaryRemaining > 0) {
          break;
        }
        continue;
      }

      state.rxBuffer = concatUint8(state.rxBuffer, value);
      value = new Uint8Array(0);

      const idx = findNewline(state.rxBuffer);
      if (idx < 0) {
        break;
      }

      const lineBytes = state.rxBuffer.slice(0, idx);
      state.rxBuffer = state.rxBuffer.slice(idx + 1);
      const line = state.textDecoder.decode(lineBytes).replace(/\r/g, "");
      handleTextLine(line);

      if (state.binaryRemaining > 0 && state.rxBuffer.length > 0) {
        const extra = state.rxBuffer;
        state.rxBuffer = new Uint8Array(0);
        value = appendBinaryChunk(extra);
        if (value.length > 0) {
          continue;
        }
      }
    }
  }
}

function startAutoPreview() {
  if (state.autoPreviewTimer) {
    return;
  }
  ui.autoPreviewBtn.textContent = "Stop Auto Preview";
  state.autoPreviewTimer = window.setInterval(async () => {
    if (!state.connected || state.previewBusy) {
      return;
    }
    state.previewBusy = true;
    await sendCommand("SNAP");
  }, 800);
}

function stopAutoPreview() {
  if (state.autoPreviewTimer) {
    clearInterval(state.autoPreviewTimer);
    state.autoPreviewTimer = null;
  }
  ui.autoPreviewBtn.textContent = "Start Auto Preview";
  state.previewBusy = false;
}

ui.connectBtn.addEventListener("click", async () => {
  try {
    await connectSerial();
  } catch (err) {
    log(`connect failed: ${err.message}`);
    await disconnectSerial();
  }
});

ui.disconnectBtn.addEventListener("click", async () => {
  await disconnectSerial();
});

ui.refreshBtn.addEventListener("click", async () => {
  await sendCommand("GET ALL");
});

ui.snapBtn.addEventListener("click", async () => {
  state.previewBusy = true;
  await sendCommand("SNAP");
});

ui.autoPreviewBtn.addEventListener("click", () => {
  if (state.autoPreviewTimer) {
    stopAutoPreview();
  } else {
    startAutoPreview();
  }
});

ui.clearLogBtn.addEventListener("click", () => {
  ui.logBox.textContent = "";
});

renderParams();
updateConnectionUi();
updateParamUiEnabled();
log("open this page in Edge or Chrome, then connect to the T23 UART");
