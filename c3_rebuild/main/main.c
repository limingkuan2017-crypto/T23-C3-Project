#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "t23_c3_protocol.h"

#define TAG "c3_bridge"

#define WIFI_STA_SSID "MK"
#define WIFI_STA_PASS "12345678"

#define C3_SPI_HOST SPI2_HOST
#define PIN_NUM_MOSI 10
#define PIN_NUM_MISO 8
#define PIN_NUM_CLK 6
#define PIN_NUM_CS 5
#define PIN_NUM_DATA_READY 3

#define T23_UART_PORT UART_NUM_1
#define T23_UART_TX 19
#define T23_UART_RX 18
#define T23_UART_BAUD 115200

#define UART_LINE_MAX 128
#define JPEG_MAX_SIZE (128 * 1024)
#define JPEG_MIN_SIZE (32 * 1024)
#define PREVIEW_REFRESH_MS 200

typedef struct {
    const char *name;
    t23_c3_param_id_t id;
} bridge_param_t;

static const bridge_param_t g_params[] = {
    { "BRIGHTNESS", T23_C3_PARAM_BRIGHTNESS },
    { "CONTRAST", T23_C3_PARAM_CONTRAST },
    { "SHARPNESS", T23_C3_PARAM_SHARPNESS },
    { "SATURATION", T23_C3_PARAM_SATURATION },
    { "AE_COMP", T23_C3_PARAM_AE_COMP },
    { "DPC", T23_C3_PARAM_DPC },
    { "DRC", T23_C3_PARAM_DRC },
    { "AWB_CT", T23_C3_PARAM_AWB_CT },
};

static SemaphoreHandle_t g_bridge_lock;
static uint8_t *g_spi_tx_buf;
static uint8_t *g_spi_rx_buf;
static uint8_t *g_latest_jpeg;
static size_t g_latest_jpeg_capacity;

static const char g_index_html[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "  <title>T23 ISP WiFi Tuner</title>\n"
    "  <link rel=\"stylesheet\" href=\"/styles.css\">\n"
    "</head>\n"
    "<body>\n"
    "  <main class=\"layout\">\n"
    "    <section class=\"panel panel--hero\">\n"
    "      <div>\n"
    "        <p class=\"eyebrow\">T23 ISP Tuning</p>\n"
    "        <h1>WiFi Web Tuner</h1>\n"
    "        <p class=\"subtitle\">This page is served by the ESP32-C3. Control commands go to T23 over UART, and JPEG snapshots come back over SPI.</p>\n"
    "      </div>\n"
    "      <div class=\"connection-card\">\n"
    "        <div class=\"status-row\"><span class=\"status-dot connected\"></span><span id=\"statusText\">Connected to C3 bridge</span></div>\n"
    "        <div class=\"button-row\">\n"
    "          <button id=\"refreshBtn\">Refresh Values</button>\n"
    "          <button id=\"pingBtn\">Ping T23</button>\n"
    "          <button id=\"snapBtn\">Capture Snapshot</button>\n"
    "          <button id=\"autoPreviewBtn\">Start Auto Preview</button>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\">\n"
    "      <div class=\"section-header\"><h2>ISP Controls</h2></div>\n"
    "      <div class=\"grid\" id=\"paramGrid\"></div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--preview\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Preview</h2>\n"
    "        <span id=\"previewStatus\" class=\"preview-status\">Idle</span>\n"
    "      </div>\n"
    "      <div class=\"preview-wrap\"><img id=\"previewImage\" alt=\"JPEG preview will appear here\"></div>\n"
    "    </section>\n"
    "    <section class=\"panel\">\n"
    "      <div class=\"section-header\"><h2>Log</h2><button id=\"clearLogBtn\">Clear</button></div>\n"
    "      <pre id=\"logBox\" class=\"log-box\"></pre>\n"
    "    </section>\n"
    "  </main>\n"
    "  <script src=\"/app.js\"></script>\n"
    "</body>\n"
    "</html>\n";

static const char g_styles_css[] =
    ":root {\n"
    "  --bg: #eef1e8;\n"
    "  --panel: #fffdf7;\n"
    "  --ink: #1e2a24;\n"
    "  --muted: #5b6b61;\n"
    "  --accent: #0d6b57;\n"
    "  --accent-2: #d26a2e;\n"
    "  --line: #d9dfd5;\n"
    "  --good: #14804a;\n"
    "  --bad: #b83b2d;\n"
    "  --shadow: 0 14px 34px rgba(19, 43, 33, 0.08);\n"
    "}\n"
    "* { box-sizing: border-box; }\n"
    "body {\n"
    "  margin: 0;\n"
    "  font-family: \"Segoe UI\", \"PingFang SC\", \"Microsoft YaHei\", sans-serif;\n"
    "  color: var(--ink);\n"
    "  background: radial-gradient(circle at top right, rgba(210, 106, 46, 0.12), transparent 28%), radial-gradient(circle at top left, rgba(13, 107, 87, 0.12), transparent 32%), var(--bg);\n"
    "}\n"
    ".layout { width: min(1200px, calc(100vw - 32px)); margin: 20px auto 40px; display: grid; gap: 18px; }\n"
    ".panel { background: var(--panel); border: 1px solid var(--line); border-radius: 18px; box-shadow: var(--shadow); padding: 20px; }\n"
    ".panel--hero { display: grid; grid-template-columns: 1.6fr 1fr; gap: 20px; align-items: start; }\n"
    ".eyebrow { margin: 0 0 8px; color: var(--accent-2); font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; font-size: 12px; }\n"
    "h1, h2 { margin: 0; }\n"
    "h1 { font-size: clamp(28px, 4vw, 42px); }\n"
    ".subtitle { color: var(--muted); line-height: 1.6; }\n"
    ".connection-card { border: 1px solid var(--line); border-radius: 16px; padding: 16px; background: linear-gradient(180deg, #ffffff, #f4f8f2); }\n"
    ".status-row, .button-row, .section-header { display: flex; align-items: center; gap: 12px; }\n"
    ".section-header { justify-content: space-between; margin-bottom: 16px; }\n"
    ".status-dot { width: 12px; height: 12px; border-radius: 50%; background: var(--bad); box-shadow: 0 0 0 5px rgba(184, 59, 45, 0.12); }\n"
    ".status-dot.connected { background: var(--good); box-shadow: 0 0 0 5px rgba(20, 128, 74, 0.12); }\n"
    ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 14px; }\n"
    ".param-card { border: 1px solid var(--line); border-radius: 14px; padding: 14px; background: #fff; }\n"
    ".param-card header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }\n"
    ".param-card label { font-weight: 700; }\n"
    ".param-card .value { color: var(--accent); font-weight: 700; }\n"
    ".param-card input[type=\"range\"] { width: 100%; }\n"
    ".param-actions { display: flex; gap: 8px; margin-top: 10px; }\n"
    "button, select { border-radius: 10px; border: 1px solid var(--line); padding: 10px 12px; font: inherit; }\n"
    "button { cursor: pointer; background: #fff; }\n"
    "button:hover:enabled { border-color: var(--accent); }\n"
    "button:disabled { opacity: 0.5; cursor: not-allowed; }\n"
    ".preview-wrap { border: 1px dashed var(--line); border-radius: 16px; min-height: 280px; background: repeating-linear-gradient(45deg, rgba(13, 107, 87, 0.03), rgba(13, 107, 87, 0.03) 16px, rgba(210, 106, 46, 0.03) 16px, rgba(210, 106, 46, 0.03) 32px); display: flex; align-items: center; justify-content: center; overflow: hidden; }\n"
    ".preview-status { display: inline-flex; align-items: center; min-height: 36px; padding: 6px 12px; border-radius: 999px; border: 1px solid var(--line); background: #f7faf4; color: var(--muted); font-weight: 700; }\n"
    ".preview-status.is-busy { color: var(--accent); border-color: rgba(13, 107, 87, 0.3); background: rgba(13, 107, 87, 0.08); }\n"
    ".preview-status.is-good { color: var(--good); border-color: rgba(20, 128, 74, 0.25); background: rgba(20, 128, 74, 0.08); }\n"
    ".preview-status.is-bad { color: var(--bad); border-color: rgba(184, 59, 45, 0.25); background: rgba(184, 59, 45, 0.08); }\n"
    "#previewImage { max-width: 100%; max-height: 560px; display: block; }\n"
    ".log-box { min-height: 180px; max-height: 300px; overflow: auto; padding: 14px; background: #122019; color: #cce7db; border-radius: 14px; white-space: pre-wrap; }\n"
    "@media (max-width: 820px) { .panel--hero { grid-template-columns: 1fr; } .button-row { flex-wrap: wrap; } }\n";

static const char g_app_js[] =
    "const PARAMS=[\n"
    " {key:'BRIGHTNESS',label:'Brightness',min:0,max:255,step:1},\n"
    " {key:'CONTRAST',label:'Contrast',min:0,max:255,step:1},\n"
    " {key:'SHARPNESS',label:'Sharpness',min:0,max:255,step:1},\n"
    " {key:'SATURATION',label:'Saturation',min:0,max:255,step:1},\n"
    " {key:'AE_COMP',label:'AE Compensation',min:90,max:250,step:1},\n"
    " {key:'DPC',label:'DPC Strength',min:0,max:255,step:1},\n"
    " {key:'DRC',label:'DRC Strength',min:0,max:255,step:1},\n"
    " {key:'AWB_CT',label:'AWB Color Temp',min:1500,max:12000,step:10}\n"
    "];\n"
    "const ui={refreshBtn:document.getElementById('refreshBtn'),pingBtn:document.getElementById('pingBtn'),snapBtn:document.getElementById('snapBtn'),autoPreviewBtn:document.getElementById('autoPreviewBtn'),clearLogBtn:document.getElementById('clearLogBtn'),paramGrid:document.getElementById('paramGrid'),logBox:document.getElementById('logBox'),previewImage:document.getElementById('previewImage'),previewStatus:document.getElementById('previewStatus')};\n"
    "const state={autoPreviewTimer:null,previewBusy:false,previewUrl:null};\n"
    "function log(m){const ts=new Date().toLocaleTimeString();ui.logBox.textContent+=`[${ts}] ${m}\\n`;ui.logBox.scrollTop=ui.logBox.scrollHeight;}\n"
    "function setPreviewStatus(t,k='idle'){ui.previewStatus.textContent=t;ui.previewStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.previewStatus.classList.add('is-busy');else if(k==='good')ui.previewStatus.classList.add('is-good');else if(k==='bad')ui.previewStatus.classList.add('is-bad');}\n"
    "function renderParams(){ui.paramGrid.innerHTML='';for(const p of PARAMS){const card=document.createElement('article');card.className='param-card';card.innerHTML=`<header><label for=\"slider-${p.key}\">${p.label}</label><span class=\"value\" id=\"value-${p.key}\">-</span></header><input id=\"slider-${p.key}\" type=\"range\" min=\"${p.min}\" max=\"${p.max}\" step=\"${p.step}\" value=\"${p.min}\"><div class=\"param-actions\"><button id=\"apply-${p.key}\">Apply</button><button id=\"read-${p.key}\">Read</button></div>`;ui.paramGrid.appendChild(card);const slider=document.getElementById(`slider-${p.key}`);const value=document.getElementById(`value-${p.key}`);document.getElementById(`apply-${p.key}`).onclick=()=>setParam(p.key,slider.value,true);document.getElementById(`read-${p.key}`).onclick=()=>refreshValues();slider.oninput=()=>{value.textContent=slider.value;};slider.onchange=()=>setParam(p.key,slider.value,true);p.slider=slider;p.valueLabel=value;}}\n"
    "function applyValues(data){for(const p of PARAMS){if(typeof data[p.key]!=='undefined'){p.slider.value=data[p.key];p.valueLabel.textContent=data[p.key];}}}\n"
    "async function fetchJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json();}\n"
    "async function refreshValues(){setPreviewStatus('Refreshing parameters...','busy');log('GET /api/params');try{const data=await fetchJson('/api/params');applyValues(data);setPreviewStatus('Parameters refreshed','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function pingT23(){log('GET /api/ping');try{await fetchJson('/api/ping');setPreviewStatus('T23 online','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setParam(key,value,autoSnap){log(`GET /api/set?key=${key}&value=${value}`);try{const data=await fetchJson(`/api/set?key=${encodeURIComponent(key)}&value=${encodeURIComponent(value)}`);applyValues(data);setPreviewStatus(`Applied ${key}`,'good');if(autoSnap)await captureSnapshot();}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function captureSnapshot(){if(state.previewBusy)return;state.previewBusy=true;setPreviewStatus('Capturing snapshot...','busy');const url=`/api/snap?t=${Date.now()}`;log(`GET ${url}`);try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();if(state.previewUrl)URL.revokeObjectURL(state.previewUrl);state.previewUrl=URL.createObjectURL(blob);ui.previewImage.src=state.previewUrl;setPreviewStatus('Preview updated','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}finally{state.previewBusy=false;}}\n"
    "function startAutoPreview(){if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';setPreviewStatus('Auto preview stopped');return;}captureSnapshot();state.autoPreviewTimer=setInterval(()=>captureSnapshot(),350);ui.autoPreviewBtn.textContent='Stop Auto Preview';setPreviewStatus('Auto preview running','good');}\n"
    "ui.refreshBtn.onclick=refreshValues;ui.pingBtn.onclick=pingT23;ui.snapBtn.onclick=captureSnapshot;ui.autoPreviewBtn.onclick=startAutoPreview;ui.clearLogBtn.onclick=()=>{ui.logBox.textContent='';};renderParams();pingT23().then(refreshValues).then(captureSnapshot);\n";

static void post_setup_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    gpio_set_level(PIN_NUM_DATA_READY, 1);
}

static void post_trans_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    gpio_set_level(PIN_NUM_DATA_READY, 0);
}

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *message, httpd_err_code_t code)
{
    char buf[192];

    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send_err(req, code, buf);
}

static const bridge_param_t *find_param(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        if (strcmp(g_params[i].name, name) == 0) {
            return &g_params[i];
        }
    }

    return NULL;
}

static void uart_flush_rx(void)
{
    uart_flush_input(T23_UART_PORT);
}

static esp_err_t uart_send_line(const char *line)
{
    int len = (int)strlen(line);

    ESP_LOGI(TAG, "UART >> %s", line);

    if (uart_write_bytes(T23_UART_PORT, line, len) < 0) {
        return ESP_FAIL;
    }
    if (uart_write_bytes(T23_UART_PORT, "\n", 1) < 0) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(T23_UART_PORT, pdMS_TO_TICKS(1000));
}

static int uart_read_line(char *buf, size_t buf_size, int timeout_ms)
{
    size_t len = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (len + 1 < buf_size) {
        char ch;
        int ret;
        TickType_t now = xTaskGetTickCount();

        if (now >= deadline) {
            break;
        }

        ret = uart_read_bytes(T23_UART_PORT, &ch, 1, pdMS_TO_TICKS(20));
        if (ret <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        buf[len++] = ch;
    }

    buf[len] = '\0';
    if (len > 0) {
        ESP_LOGI(TAG, "UART << %s", buf);
    }
    return (int)len;
}

static esp_err_t init_t23_uart(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = T23_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(T23_UART_PORT, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(T23_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(T23_UART_PORT, T23_UART_TX, T23_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "T23 UART ready on UART%d tx=%d rx=%d baud=%d",
             (int)T23_UART_PORT, T23_UART_TX, T23_UART_RX, T23_UART_BAUD);
    return ESP_OK;
}

static esp_err_t bridge_ping(void)
{
    char line[UART_LINE_MAX];
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("PING"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "PING timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (strcmp(line, "PONG") != 0) {
        ESP_LOGW(TAG, "unexpected ping response: %s", line);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bridge_get_all(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    int values[sizeof(g_params) / sizeof(g_params[0])] = {0};
    int have_value[sizeof(g_params) / sizeof(g_params[0])] = {0};
    size_t used = 0;
    int len;
    size_t i;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("GET ALL"));

    while (1) {
        char key[32];
        int value;

        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "GET ALL timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(line, "OK GET ALL") == 0) {
            break;
        }
        if (sscanf(line, "VAL %31s %d", key, &value) == 2) {
            for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
                if (strcmp(g_params[i].name, key) == 0) {
                    values[i] = value;
                    have_value[i] = 1;
                    break;
                }
            }
            continue;
        }
        ESP_LOGW(TAG, "GET ALL unexpected line: %s", line);
    }

    used += (size_t)snprintf(json_buf + used, json_buf_size - used, "{\"ok\":true");
    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        if (!have_value[i]) {
            continue;
        }
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 ",\"%s\":%d",
                                 g_params[i].name,
                                 values[i]);
    }
    snprintf(json_buf + used, json_buf_size - used, "}");
    return ESP_OK;
}

static esp_err_t bridge_set_param(const char *key, int value, char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char cmd[64];
    int actual_value = value;
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "SET %s %d", key, value);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        char echoed_key[32];
        int echoed_value;

        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }

        if (sscanf(line, "VAL %31s %d", echoed_key, &echoed_value) == 2) {
            if (strcmp(echoed_key, key) == 0) {
                actual_value = echoed_value;
            }
            continue;
        }

        if (strncmp(line, "OK SET ", 7) == 0) {
            snprintf(json_buf,
                     json_buf_size,
                     "{\"ok\":true,\"key\":\"%s\",\"value\":%d}",
                     key,
                     actual_value);
            return ESP_OK;
        }

        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "SET failed: %s", line);
            return ESP_FAIL;
        }
    }
}

static esp_err_t init_spi_slave(void)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = T23_C3_SPI_FRAME_SIZE,
    };
    const spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
        .flags = 0,
        .post_setup_cb = post_setup_cb,
        .post_trans_cb = post_trans_cb,
    };
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_NUM_DATA_READY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(PIN_NUM_DATA_READY, 0));

    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(spi_slave_initialize(C3_SPI_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));

    g_spi_tx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, T23_C3_SPI_FRAME_SIZE, 0);
    g_spi_rx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, T23_C3_SPI_FRAME_SIZE, 0);
    assert(g_spi_tx_buf && g_spi_rx_buf);

    ESP_LOGI(TAG, "SPI slave ready");
    ESP_LOGI(TAG, "pins: mosi=%d miso=%d clk=%d cs=%d dr=%d",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS, PIN_NUM_DATA_READY);
    return ESP_OK;
}

static esp_err_t spi_receive_frame(t23_c3_frame_t *frame, int timeout_ms)
{
    spi_slave_transaction_t t;
    esp_err_t ret;

    memset(g_spi_tx_buf, 0, T23_C3_SPI_FRAME_SIZE);
    memset(g_spi_rx_buf, 0, T23_C3_SPI_FRAME_SIZE);
    memset(&t, 0, sizeof(t));
    t.length = T23_C3_SPI_FRAME_SIZE * 8;
    t.tx_buffer = g_spi_tx_buf;
    t.rx_buffer = g_spi_rx_buf;

    ret = spi_slave_transmit(C3_SPI_HOST, &t, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(frame, g_spi_rx_buf, sizeof(*frame));
    return ESP_OK;
}

static esp_err_t bridge_fetch_snapshot_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    char line[UART_LINE_MAX];
    int len;
    uint32_t jpeg_len = 0;
    uint32_t received = 0;

    *jpeg_len_out = 0;
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("SNAP"));

    while (1) {
        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "SNAP timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "SNAP OK %" SCNu32, &jpeg_len) == 1) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "SNAP failed: %s", line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "SNAP unexpected UART line: %s", line);
    }

    if (jpeg_len == 0 || jpeg_len > jpeg_buf_size) {
        ESP_LOGW(TAG, "invalid jpeg length: %" PRIu32, jpeg_len);
        return ESP_FAIL;
    }

    while (received < jpeg_len) {
        t23_c3_frame_t frame;
        esp_err_t ret = spi_receive_frame(&frame, 3000);

        if (ret != ESP_OK) {
            return ret;
        }
        if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
            ESP_LOGW(TAG, "SPI bad frame magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
            return ESP_FAIL;
        }
        if (frame.hdr.type == T23_C3_FRAME_TYPE_RESP_JPEG_INFO) {
            continue;
        }
        if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_JPEG_DATA) {
            ESP_LOGW(TAG, "SPI unexpected frame type: %u", frame.hdr.type);
            return ESP_FAIL;
        }
        if (frame.hdr.payload_len > T23_C3_FRAME_PAYLOAD_MAX) {
            return ESP_FAIL;
        }
        if (received + frame.hdr.payload_len > jpeg_len) {
            return ESP_FAIL;
        }

        memcpy(jpeg_buf + received, frame.payload, frame.hdr.payload_len);
        received += frame.hdr.payload_len;
    }

    *jpeg_len_out = received;
    return ESP_OK;
}

static size_t choose_preview_capacity(void)
{
    static const size_t candidates[] = {
        128 * 1024,
        96 * 1024,
        64 * 1024,
        48 * 1024,
        32 * 1024,
    };
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint8_t *buf = malloc(candidates[i]);

        if (buf != NULL) {
            g_latest_jpeg = buf;
            g_latest_jpeg_capacity = candidates[i];
            return candidates[i];
        }
    }

    return 0;
}

static esp_err_t bridge_stream_snapshot(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    ret = bridge_fetch_snapshot_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, g_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, g_app_js, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t styles_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    return httpd_resp_send(req, g_styles_css, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ping_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_ping();
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "ping failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, "{\"ok\":true,\"pong\":true}");
}

static esp_err_t params_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[512];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_all(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "get_all failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t set_handler(httpd_req_t *req)
{
    char query[128];
    char key[32];
    char value_str[16];
    char json[128];
    const bridge_param_t *param;
    int value;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "key", key, sizeof(key)) != ESP_OK) {
        return send_error_json(req, "missing key", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value_str, sizeof(value_str)) != ESP_OK) {
        return send_error_json(req, "missing value", HTTPD_400_BAD_REQUEST);
    }

    param = find_param(key);
    if (param == NULL) {
        return send_error_json(req, "unknown parameter", HTTPD_400_BAD_REQUEST);
    }

    value = atoi(value_str);

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_param(param->name, value, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t snap_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_snapshot(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "snap failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = app_js_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t styles = {
        .uri = "/styles.css",
        .method = HTTP_GET,
        .handler = styles_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ping = {
        .uri = "/api/ping",
        .method = HTTP_GET,
        .handler = ping_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t params = {
        .uri = "/api/params",
        .method = HTTP_GET,
        .handler = params_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t set = {
        .uri = "/api/set",
        .method = HTTP_GET,
        .handler = set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snap = {
        .uri = "/api/snap",
        .method = HTTP_GET,
        .handler = snap_handler,
        .user_ctx = NULL,
    };
    config.stack_size = 8192;
    config.max_uri_handlers = 8;

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &app_js));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &styles));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ping));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snap));

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA start, connecting to SSID \"%s\"", WIFI_STA_SSID);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_wifi_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", WIFI_STA_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", WIFI_STA_PASS);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_bridge_lock = xSemaphoreCreateMutex();
    assert(g_bridge_lock != NULL);
    if (choose_preview_capacity() == 0) {
        ESP_LOGE(TAG, "failed to allocate preview cache");
        return;
    }
    ESP_LOGI(TAG, "preview cache allocated: %u bytes", (unsigned)g_latest_jpeg_capacity);

    init_wifi_sta();
    init_t23_uart();
    init_spi_slave();
    start_http_server();
}
