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

#include "t23_border_pipeline.h"
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

#define UART_LINE_MAX 256
#define JPEG_MAX_SIZE (128 * 1024)
#define JPEG_MIN_SIZE (32 * 1024)
#define PREVIEW_REFRESH_MS 200

typedef struct {
    const char *name;
    t23_c3_param_id_t id;
} bridge_param_t;

typedef enum {
    C3_MODE_DEBUG = 0,
    C3_MODE_RUN = 1,
} c3_mode_t;

typedef struct {
    const char *name;
    int top_blocks;
    int right_blocks;
    int bottom_blocks;
    int left_blocks;
} border_layout_desc_t;

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
static char g_latest_border_json[4096];
static int g_latest_border_json_valid;
static volatile c3_mode_t g_c3_mode = C3_MODE_DEBUG;
static const border_layout_desc_t g_layout_16x9 = { "16X9", 16, 9, 16, 9 };
static const border_layout_desc_t g_layout_4x3 = { "4X3", 4, 3, 4, 3 };
static const border_layout_desc_t *g_current_layout = &g_layout_16x9;

static esp_err_t spi_receive_frame(t23_c3_frame_t *frame, int timeout_ms);

static const char *c3_mode_name(c3_mode_t mode)
{
    return (mode == C3_MODE_RUN) ? "RUN" : "DEBUG";
}

static const border_layout_desc_t *find_layout_desc(const char *name)
{
    if (name != NULL && strcmp(name, g_layout_4x3.name) == 0) {
        return &g_layout_4x3;
    }
    return &g_layout_16x9;
}

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
    "        <div class=\"status-row\"><span class=\"status-dot connected\"></span><span id=\"statusText\">Connected to C3 bridge</span><span id=\"modeBadge\" class=\"preview-status\">Mode: DEBUG</span></div>\n"
    "        <div class=\"button-row\">\n"
    "          <label for=\"layoutSelect\">Border Layout</label>\n"
    "          <select id=\"layoutSelect\">\n"
    "            <option value=\"16X9\">16 x 9</option>\n"
    "            <option value=\"4X3\">4 x 3</option>\n"
    "          </select>\n"
    "        </div>\n"
    "        <div class=\"button-row\">\n"
    "          <button id=\"refreshBtn\">Refresh Values</button>\n"
    "          <button id=\"pingBtn\">Ping T23</button>\n"
    "          <button id=\"snapBtn\">Capture Snapshot</button>\n"
    "          <button id=\"autoPreviewBtn\">Start Auto Preview</button>\n"
    "          <button id=\"enterRunBtn\">Enter Run Mode</button>\n"
    "          <button id=\"returnDebugBtn\">Return To Debug Mode</button>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\" id=\"ispPanel\">\n"
    "      <div class=\"section-header\"><h2>ISP Controls</h2></div>\n"
    "      <div class=\"grid\" id=\"paramGrid\"></div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--preview\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Preview</h2>\n"
    "        <span id=\"previewStatus\" class=\"preview-status\">Idle</span>\n"
    "      </div>\n"
    "      <div class=\"preview-compare\">\n"
    "        <div class=\"preview-pane\" id=\"originalPane\">\n"
          "          <h3>Original Stream</h3>\n"
          "          <div class=\"preview-wrap\"><img id=\"previewImage\" alt=\"JPEG preview will appear here\"></div>\n"
          "        </div>\n"
    "        <div class=\"preview-pane\">\n"
    "          <h3>Border Average Preview</h3>\n"
    "          <div class=\"preview-wrap\"><canvas id=\"borderCanvas\" width=\"640\" height=\"320\"></canvas></div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--calibration\" id=\"calibrationPanel\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Border Calibration</h2>\n"
    "        <span class=\"preview-status\" id=\"calibrationStatus\">Idle</span>\n"
    "      </div>\n"
    "      <p class=\"subtitle\">Drag 8 points around the TV border: 4 corners and 4 edge midpoints. Click confirm to store the calibration in T23 and fetch a real rectified preview rendered inside T23.</p>\n"
    "      <div class=\"button-row calibration-actions\">\n"
    "        <button id=\"loadCalibrationSnapBtn\">Load Calibration Snapshot</button>\n"
    "        <button id=\"loadCalibrationBtn\">Load Saved Points</button>\n"
    "        <button id=\"resetCalibrationBtn\">Reset 8 Points</button>\n"
    "        <button id=\"saveCalibrationBtn\">Confirm 8 Points</button>\n"
    "      </div>\n"
    "      <div class=\"calibration-grid\">\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Interactive Calibration</h3>\n"
    "          <canvas id=\"calibrationCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "        </div>\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Rectified Preview</h3>\n"
    "          <canvas id=\"rectifiedCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "        </div>\n"
    "      </div>\n"
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
    ".preview-compare { display:grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }\n"
    ".preview-pane h3 { margin: 0 0 10px; font-size: 18px; }\n"
    ".calibration-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }\n"
    ".calibration-pane h3 { margin: 0 0 10px; font-size: 18px; }\n"
    ".calibration-pane canvas { width: 100%; height: auto; border: 1px dashed var(--line); border-radius: 16px; background: #fbfcf8; display: block; }\n"
    ".calibration-actions { margin-bottom: 16px; flex-wrap: wrap; }\n"
    ".preview-status { display: inline-flex; align-items: center; min-height: 36px; padding: 6px 12px; border-radius: 999px; border: 1px solid var(--line); background: #f7faf4; color: var(--muted); font-weight: 700; }\n"
    ".preview-status.is-busy { color: var(--accent); border-color: rgba(13, 107, 87, 0.3); background: rgba(13, 107, 87, 0.08); }\n"
    ".preview-status.is-good { color: var(--good); border-color: rgba(20, 128, 74, 0.25); background: rgba(20, 128, 74, 0.08); }\n"
    ".preview-status.is-bad { color: var(--bad); border-color: rgba(184, 59, 45, 0.25); background: rgba(184, 59, 45, 0.08); }\n"
    ".is-hidden { display: none !important; }\n"
    "#previewImage, #borderCanvas { max-width: 100%; max-height: 560px; display: block; }\n"
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
    "const POINT_LABELS=['TL','TM','TR','RM','BR','BM','BL','LM'];\n"
    "const ui={refreshBtn:document.getElementById('refreshBtn'),pingBtn:document.getElementById('pingBtn'),snapBtn:document.getElementById('snapBtn'),autoPreviewBtn:document.getElementById('autoPreviewBtn'),enterRunBtn:document.getElementById('enterRunBtn'),returnDebugBtn:document.getElementById('returnDebugBtn'),modeBadge:document.getElementById('modeBadge'),layoutSelect:document.getElementById('layoutSelect'),ispPanel:document.getElementById('ispPanel'),originalPane:document.getElementById('originalPane'),calibrationPanel:document.getElementById('calibrationPanel'),clearLogBtn:document.getElementById('clearLogBtn'),paramGrid:document.getElementById('paramGrid'),logBox:document.getElementById('logBox'),previewImage:document.getElementById('previewImage'),borderCanvas:document.getElementById('borderCanvas'),previewStatus:document.getElementById('previewStatus'),loadCalibrationSnapBtn:document.getElementById('loadCalibrationSnapBtn'),loadCalibrationBtn:document.getElementById('loadCalibrationBtn'),resetCalibrationBtn:document.getElementById('resetCalibrationBtn'),saveCalibrationBtn:document.getElementById('saveCalibrationBtn'),calibrationStatus:document.getElementById('calibrationStatus'),calibrationCanvas:document.getElementById('calibrationCanvas'),rectifiedCanvas:document.getElementById('rectifiedCanvas')};\n"
    "const state={autoPreviewTimer:null,previewBusy:false,runtimeBusy:false,previewUrl:null,calibrationImage:null,rectifiedImage:null,borderData:null,mode:'DEBUG',layout:'16X9',lastBlocksAt:0,calibration:{imageWidth:640,imageHeight:320,points:[]},dragIndex:-1};\n"
    "function log(m){const ts=new Date().toLocaleTimeString();ui.logBox.textContent+=`[${ts}] ${m}\\n`;ui.logBox.scrollTop=ui.logBox.scrollHeight;}\n"
    "function setPreviewStatus(t,k='idle'){ui.previewStatus.textContent=t;ui.previewStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.previewStatus.classList.add('is-busy');else if(k==='good')ui.previewStatus.classList.add('is-good');else if(k==='bad')ui.previewStatus.classList.add('is-bad');}\n"
    "function setCalibrationStatus(t,k='idle'){ui.calibrationStatus.textContent=t;ui.calibrationStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.calibrationStatus.classList.add('is-busy');else if(k==='good')ui.calibrationStatus.classList.add('is-good');else if(k==='bad')ui.calibrationStatus.classList.add('is-bad');}\n"
    "function waitForImage(img){if(img.complete&&img.naturalWidth>0)return Promise.resolve();return new Promise((resolve,reject)=>{const onLoad=()=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);resolve();};const onError=(e)=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);reject(e);};img.addEventListener('load',onLoad,{once:true});img.addEventListener('error',onError,{once:true});});}\n"
    "function applyLayoutMeta(data){if(!data)return;const layout=data.layout||state.layout||'16X9';state.layout=layout;if(ui.layoutSelect&&ui.layoutSelect.value!==layout)ui.layoutSelect.value=layout;}\n"
    "function getLayoutMeta(d){const top=d.topBlocks||16,right=d.rightBlocks||9,bottom=d.bottomBlocks||16,left=d.leftBlocks||9;return{top,right,bottom,left,total:(d.blockCount||d.blocks?.length||0),layout:(d.layout||state.layout||'16X9')};}\n"
    "function setModeUi(mode){state.mode=mode;ui.modeBadge.textContent=`Mode: ${mode}`;const isRun=mode==='RUN';ui.originalPane.classList.toggle('is-hidden',isRun);ui.ispPanel.classList.toggle('is-hidden',isRun);ui.calibrationPanel.classList.toggle('is-hidden',isRun);ui.snapBtn.disabled=isRun;ui.enterRunBtn.disabled=isRun;ui.returnDebugBtn.disabled=!isRun;if(isRun){setPreviewStatus('Run mode active','good');}else{setPreviewStatus('Debug mode active','good');}}\n"
    "function renderParams(){ui.paramGrid.innerHTML='';for(const p of PARAMS){const card=document.createElement('article');card.className='param-card';card.innerHTML=`<header><label for=\"slider-${p.key}\">${p.label}</label><span class=\"value\" id=\"value-${p.key}\">-</span></header><input id=\"slider-${p.key}\" type=\"range\" min=\"${p.min}\" max=\"${p.max}\" step=\"${p.step}\" value=\"${p.min}\"><div class=\"param-actions\"><button id=\"apply-${p.key}\">Apply</button><button id=\"read-${p.key}\">Read</button></div>`;ui.paramGrid.appendChild(card);const slider=document.getElementById(`slider-${p.key}`);const value=document.getElementById(`value-${p.key}`);document.getElementById(`apply-${p.key}`).onclick=()=>setParam(p.key,slider.value,true);document.getElementById(`read-${p.key}`).onclick=()=>refreshValues();slider.oninput=()=>{value.textContent=slider.value;};slider.onchange=()=>setParam(p.key,slider.value,true);p.slider=slider;p.valueLabel=value;}}\n"
    "function applyValues(data){for(const p of PARAMS){if(typeof data[p.key]!=='undefined'){p.slider.value=data[p.key];p.valueLabel.textContent=data[p.key];}}}\n"
    "function makeDefaultCalibration(w,h){const left=Math.round(w/8),right=Math.round(w-left),top=Math.round(h/8),bottom=Math.round(h-top),cx=Math.round(w/2),cy=Math.round(h/2);return{imageWidth:w,imageHeight:h,points:[{x:left,y:top},{x:cx,y:top},{x:right,y:top},{x:right,y:cy},{x:right,y:bottom},{x:cx,y:bottom},{x:left,y:bottom},{x:left,y:cy}]};}\n"
    "function ensureCalibrationPoints(){if(!state.calibration.points||state.calibration.points.length!==8){state.calibration=makeDefaultCalibration(state.calibration.imageWidth||640,state.calibration.imageHeight||320);}}\n"
    "function drawCalibrationCanvas(){const c=ui.calibrationCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,c.width,c.height);if(state.calibrationImage){ctx.drawImage(state.calibrationImage,0,0,c.width,c.height);}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ensureCalibrationPoints();ctx.beginPath();state.calibration.points.forEach((p,i)=>{if(i===0)ctx.moveTo(p.x,p.y);else ctx.lineTo(p.x,p.y);});ctx.closePath();ctx.stroke();ctx.font='12px sans-serif';state.calibration.points.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#d26a2e';ctx.arc(p.x,p.y,7,0,Math.PI*2);ctx.fill();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p.x+10,p.y-10);});}\n"
    "function drawRectifiedGuide(){const c=ui.rectifiedCanvas,ctx=c.getContext('2d');const pad=40,w=c.width,h=c.height;const left=pad,right=w-pad,top=pad,bottom=h-pad,cx=Math.round((left+right)/2),cy=Math.round((top+bottom)/2);ctx.clearRect(0,0,w,h);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,w,h);if(state.rectifiedImage){const img=state.rectifiedImage;const scale=Math.min(w/img.width,h/img.height);const dw=Math.round(img.width*scale),dh=Math.round(img.height*scale);const dx=Math.round((w-dw)/2),dy=Math.round((h-dh)/2);ctx.drawImage(img,dx,dy,dw,dh);ctx.strokeStyle='rgba(13,107,87,0.45)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,dw,dh);ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview rendered by T23',pad,h-12);return;}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ctx.strokeRect(left,top,right-left,bottom-top);const pts=[[left,top],[cx,top],[right,top],[right,cy],[right,bottom],[cx,bottom],[left,bottom],[left,cy]];ctx.font='12px sans-serif';pts.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#0d6b57';ctx.arc(p[0],p[1],7,0,Math.PI*2);ctx.fill();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p[0]+10,p[1]-10);});ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview will appear here after calibration confirm',pad,h-12);}\n"
    "function drawBorderBlocks(){const c=ui.borderCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);if(!state.borderData){ctx.fillStyle='#cce7db';ctx.font='16px sans-serif';ctx.fillText('Border average preview will appear here',20,30);return;}const d=state.borderData,m=getLayoutMeta(d);const margin=16,outerW=c.width-margin*2,outerH=c.height-margin*2;const l=margin,t=margin,r=l+outerW,b=t+outerH;const topCell=Math.max(1,outerW/Math.max(1,m.top));const sideCell=Math.max(1,outerH/Math.max(1,m.right));const th=Math.max(8,Math.min(Math.round(Math.min(topCell,sideCell)*0.85),Math.round(Math.min(outerW,outerH)*0.42)));const innerTop=t+th,innerBottom=b-th,innerLeft=l+th,innerRight=r-th;let idx=0;for(let i=0;i<m.top&&idx<d.blocks.length;i++,idx++){const x0=Math.round(l+outerW*i/m.top),x1=Math.round(l+outerW*(i+1)/m.top);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(x0,t,Math.max(1,x1-x0),th);}const rightStart=idx;if(m.layout==='4X3'&&m.right===3&&rightStart+1<d.blocks.length){const c0=d.blocks[rightStart+1];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(r-th,innerTop,th,Math.max(1,innerBottom-innerTop));idx+=m.right;}else{for(let i=0;i<m.right&&idx<d.blocks.length;i++,idx++){const y0=Math.round(innerTop+(innerBottom-innerTop)*i/m.right),y1=Math.round(innerTop+(innerBottom-innerTop)*(i+1)/m.right);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(r-th,y0,th,Math.max(1,y1-y0));}}for(let i=0;i<m.bottom&&idx<d.blocks.length;i++,idx++){const x0=Math.round(l+outerW*(m.bottom-1-i)/m.bottom),x1=Math.round(l+outerW*(m.bottom-i)/m.bottom);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(x0,b-th,Math.max(1,x1-x0),th);}const leftStart=idx;if(m.layout==='4X3'&&m.left===3&&leftStart+1<d.blocks.length){const c0=d.blocks[leftStart+1];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(l,innerTop,th,Math.max(1,innerBottom-innerTop));idx+=m.left;}else{for(let i=0;i<m.left&&idx<d.blocks.length;i++,idx++){const y0=Math.round(innerTop+(innerBottom-innerTop)*(m.left-1-i)/m.left),y1=Math.round(innerTop+(innerBottom-innerTop)*(m.left-i)/m.left);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(l,y0,th,Math.max(1,y1-y0));}}ctx.fillStyle='#101814';ctx.fillRect(innerLeft,innerTop,Math.max(1,innerRight-innerLeft),Math.max(1,innerBottom-innerTop));ctx.strokeStyle='rgba(255,255,255,0.35)';ctx.lineWidth=2;ctx.strokeRect(l,t,Math.max(1,outerW),Math.max(1,outerH));ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';ctx.fillText(`${m.total} block average colors (${m.layout}) rendered from T23 output`,16,c.height-12);}\n"
    "function canvasPos(evt){const rect=ui.calibrationCanvas.getBoundingClientRect();const sx=ui.calibrationCanvas.width/rect.width,sy=ui.calibrationCanvas.height/rect.height;return{x:(evt.clientX-rect.left)*sx,y:(evt.clientY-rect.top)*sy};}\n"
    "function pickPoint(pos){ensureCalibrationPoints();for(let i=0;i<state.calibration.points.length;i++){const p=state.calibration.points[i];const dx=p.x-pos.x,dy=p.y-pos.y;if((dx*dx+dy*dy)<=225)return i;}return -1;}\n"
    "function clamp(v,min,max){return Math.max(min,Math.min(max,v));}\n"
    "function bindCalibrationCanvas(){ui.calibrationCanvas.onpointerdown=(evt)=>{const pos=canvasPos(evt);state.dragIndex=pickPoint(pos);if(state.dragIndex>=0){ui.calibrationCanvas.setPointerCapture(evt.pointerId);}};ui.calibrationCanvas.onpointermove=(evt)=>{if(state.dragIndex<0)return;const pos=canvasPos(evt);const maxX=state.calibration.imageWidth-1,maxY=state.calibration.imageHeight-1;state.calibration.points[state.dragIndex]={x:Math.round(clamp(pos.x,0,maxX)),y:Math.round(clamp(pos.y,0,maxY))};drawCalibrationCanvas();drawRectifiedGuide();};ui.calibrationCanvas.onpointerup=(evt)=>{state.dragIndex=-1;try{ui.calibrationCanvas.releasePointerCapture(evt.pointerId);}catch(_){};};ui.calibrationCanvas.onpointerleave=()=>{};}\n"
    "async function loadCalibrationSnapshot(){setCalibrationStatus('Loading snapshot...','busy');try{const r=await fetch(`/api/snap?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.calibrationImage=img;ui.calibrationCanvas.width=img.width;ui.calibrationCanvas.height=img.height;if(!state.calibration.points||state.calibration.imageWidth!==img.width||state.calibration.imageHeight!==img.height){state.calibration=makeDefaultCalibration(img.width,img.height);}drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration snapshot ready','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadRectifiedPreview(){setCalibrationStatus('Loading rectified preview...','busy');log(`GET /api/calibration/rectified?t=${Date.now()}`);try{const r=await fetch(`/api/calibration/rectified?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.rectifiedImage=img;drawRectifiedGuide();setCalibrationStatus('Rectified preview updated','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadCalibration(){setCalibrationStatus('Loading saved points...','busy');log('GET /api/calibration');try{const data=await fetchJson('/api/calibration');state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};if(!state.calibrationImage)await loadCalibrationSnapshot();else{ui.calibrationCanvas.width=state.calibration.imageWidth;ui.calibrationCanvas.height=state.calibration.imageHeight;drawCalibrationCanvas();drawRectifiedGuide();}await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "function resetCalibration(){const w=(state.calibrationImage&&state.calibrationImage.width)||state.calibration.imageWidth||640;const h=(state.calibrationImage&&state.calibrationImage.height)||state.calibration.imageHeight||320;state.calibration=makeDefaultCalibration(w,h);state.rectifiedImage=null;drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration points reset','good');}\n"
    "async function saveCalibration(){ensureCalibrationPoints();const q=new URLSearchParams();q.set('iw',state.calibration.imageWidth);q.set('ih',state.calibration.imageHeight);state.calibration.points.forEach((p,i)=>{q.set(`x${i}`,p.x);q.set(`y${i}`,p.y);});const url=`/api/calibration/set?${q.toString()}`;setCalibrationStatus('Saving calibration...','busy');log(`GET ${url}`);try{const data=await fetchJson(url);state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};drawCalibrationCanvas();state.rectifiedImage=null;drawRectifiedGuide();await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function fetchJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json();}\n"
    "async function refreshLayout(){log('GET /api/layout');try{const data=await fetchJson('/api/layout');applyLayoutMeta(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function setLayout(value){log(`GET /api/layout/set?value=${value}`);try{const data=await fetchJson(`/api/layout/set?value=${encodeURIComponent(value)}`);applyLayoutMeta(data);drawBorderBlocks();if(state.mode==='RUN')await refreshRuntimeBlocks();else await refreshPreviewAndBlocks();}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshMode(){log('GET /api/mode');try{const data=await fetchJson('/api/mode');setModeUi(data.mode||'DEBUG');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setMode(mode){log(`GET /api/mode/set?value=${mode}`);try{const data=await fetchJson(`/api/mode/set?value=${encodeURIComponent(mode)}`);setModeUi(data.mode||mode);if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';}if(mode==='RUN'){startAutoPreview();}}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshValues(){setPreviewStatus('Refreshing parameters...','busy');log('GET /api/params');try{const data=await fetchJson('/api/params');applyValues(data);setPreviewStatus('Parameters refreshed','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function pingT23(){log('GET /api/ping');try{await fetchJson('/api/ping');setPreviewStatus('T23 online','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setParam(key,value,autoSnap){log(`GET /api/set?key=${key}&value=${value}`);try{const data=await fetchJson(`/api/set?key=${encodeURIComponent(key)}&value=${encodeURIComponent(value)}`);applyValues(data);setPreviewStatus(`Applied ${key}`,'good');if(autoSnap){await refreshPreviewAndBlocks();}}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshBorderBlocks(){log('GET /api/border_blocks');try{state.borderData=await fetchJson('/api/border_blocks');applyLayoutMeta(state.borderData);state.lastBlocksAt=Date.now();drawBorderBlocks();}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function refreshRuntimeBlocks(){if(state.runtimeBusy)return;state.runtimeBusy=true;log('GET /api/runtime_blocks');try{state.borderData=await fetchJson('/api/runtime_blocks');applyLayoutMeta(state.borderData);drawBorderBlocks();setPreviewStatus('Run mode blocks updated','good');}catch(e){log(`ERR ${e.message}`);if(String(e.message).includes('HTTP 500'))setPreviewStatus('Run mode warming up...','busy');else setPreviewStatus(`ERR ${e.message}`,'bad');}finally{state.runtimeBusy=false;}}\n"
    "async function captureSnapshot(){if(state.previewBusy)return false;state.previewBusy=true;setPreviewStatus('Capturing snapshot...','busy');const url=`/api/snap?t=${Date.now()}`;log(`GET ${url}`);try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();if(state.previewUrl)URL.revokeObjectURL(state.previewUrl);state.previewUrl=URL.createObjectURL(blob);ui.previewImage.src=state.previewUrl;await waitForImage(ui.previewImage);setPreviewStatus('Preview updated','good');return true;}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');return false;}finally{state.previewBusy=false;}}\n"
    "async function refreshPreviewAndBlocks(){const ok=await captureSnapshot();if(ok)await refreshBorderBlocks();}\n"
    "function startAutoPreview(){if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';setPreviewStatus('Auto preview stopped');return;}const runner=(state.mode==='RUN')?refreshRuntimeBlocks:refreshPreviewAndBlocks;runner();state.autoPreviewTimer=setInterval(()=>runner(),state.mode==='RUN'?120:280);ui.autoPreviewBtn.textContent='Stop Auto Preview';setPreviewStatus(state.mode==='RUN'?'Run mode refresh active':'Auto preview running','good');}\n"
    "ui.refreshBtn.onclick=refreshValues;ui.pingBtn.onclick=pingT23;ui.snapBtn.onclick=refreshPreviewAndBlocks;ui.autoPreviewBtn.onclick=startAutoPreview;ui.enterRunBtn.onclick=()=>setMode('RUN');ui.returnDebugBtn.onclick=()=>setMode('DEBUG');ui.layoutSelect.onchange=()=>setLayout(ui.layoutSelect.value);ui.clearLogBtn.onclick=()=>{ui.logBox.textContent='';};ui.loadCalibrationSnapBtn.onclick=loadCalibrationSnapshot;ui.loadCalibrationBtn.onclick=loadCalibration;ui.resetCalibrationBtn.onclick=resetCalibration;ui.saveCalibrationBtn.onclick=saveCalibration;renderParams();bindCalibrationCanvas();resetCalibration();drawRectifiedGuide();drawBorderBlocks();pingT23().then(refreshMode).then(refreshLayout).then(()=>state.mode==='RUN'?refreshRuntimeBlocks():refreshValues().then(refreshPreviewAndBlocks).then(loadCalibration)).catch(()=>{});\n";

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

static esp_err_t bridge_get_mode(c3_mode_t *mode_out)
{
    char line[UART_LINE_MAX];
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("MODE GET"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "MODE GET timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (strcmp(line, "MODE RUN") == 0) {
        *mode_out = C3_MODE_RUN;
        return ESP_OK;
    }
    if (strcmp(line, "MODE DEBUG") == 0) {
        *mode_out = C3_MODE_DEBUG;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "unexpected mode response: %s", line);
    return ESP_FAIL;
}

static esp_err_t bridge_set_mode(c3_mode_t mode, char *json_buf, size_t json_buf_size)
{
    char cmd[32];
    char line[UART_LINE_MAX];
    const char *target = c3_mode_name(mode);
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "MODE SET %s", target);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "MODE SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strncmp(line, "MODE ", 5) == 0) {
            continue;
        }
        if (strcmp(line, "OK MODE SET RUN") == 0) {
            g_c3_mode = C3_MODE_RUN;
            snprintf(json_buf, json_buf_size, "{\"ok\":true,\"mode\":\"RUN\"}");
            return ESP_OK;
        }
        if (strcmp(line, "OK MODE SET DEBUG") == 0) {
            g_c3_mode = C3_MODE_DEBUG;
            snprintf(json_buf, json_buf_size, "{\"ok\":true,\"mode\":\"DEBUG\"}");
            return ESP_OK;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "MODE SET failed: %s", line);
            return ESP_FAIL;
        }
    }
}

static esp_err_t bridge_get_layout(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char layout_name[16];
    int block_count = 0;
    int top_blocks = 0;
    int right_blocks = 0;
    int bottom_blocks = 0;
    int left_blocks = 0;
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("LAYOUT GET"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "LAYOUT GET timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (sscanf(line,
               "LAYOUT %15s %d %d %d %d %d",
               layout_name,
               &block_count,
               &top_blocks,
               &right_blocks,
               &bottom_blocks,
               &left_blocks) != 6) {
        ESP_LOGW(TAG, "unexpected layout response: %s", line);
        return ESP_FAIL;
    }

    g_current_layout = find_layout_desc(layout_name);
    snprintf(json_buf,
             json_buf_size,
             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d}",
             layout_name,
             block_count,
             top_blocks,
             right_blocks,
             bottom_blocks,
             left_blocks);
    return ESP_OK;
}

static esp_err_t bridge_set_layout(const char *layout_name, char *json_buf, size_t json_buf_size)
{
    char cmd[32];
    char line[UART_LINE_MAX];
    int block_count = 0;
    int top_blocks = 0;
    int right_blocks = 0;
    int bottom_blocks = 0;
    int left_blocks = 0;
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "LAYOUT SET %s", layout_name);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "LAYOUT SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line,
                   "LAYOUT %15s %d %d %d %d %d",
                   cmd,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            g_current_layout = find_layout_desc(cmd);
            continue;
        }
        if (strncmp(line, "OK LAYOUT SET ", 14) == 0) {
            snprintf(json_buf,
                     json_buf_size,
                     "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d}",
                     g_current_layout->name,
                     block_count,
                     top_blocks,
                     right_blocks,
                     bottom_blocks,
                     left_blocks);
            return ESP_OK;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "LAYOUT SET failed: %s", line);
            return ESP_FAIL;
        }
    }
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

static esp_err_t bridge_get_border_blocks(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char layout_name[16] = "16X9";
    int image_w = 640;
    int image_h = 320;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 639;
    int rect_bottom = 319;
    int thickness = 8;
    int top_blocks = g_current_layout->top_blocks;
    int right_blocks = g_current_layout->right_blocks;
    int bottom_blocks = g_current_layout->bottom_blocks;
    int left_blocks = g_current_layout->left_blocks;
    int block_count = top_blocks + right_blocks + bottom_blocks + left_blocks;
    int blocks[T23_BORDER_BLOCK_COUNT_MAX][3];
    int got_blocks[T23_BORDER_BLOCK_COUNT_MAX] = {0};
    int len;
    int i;
    size_t used = 0;

    memset(blocks, 0, sizeof(blocks));
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("BLOCKS GET"));

    while (1) {
        int idx;
        int r;
        int g;
        int b;

        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "BLOCKS GET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "BLOCKS SIZE %d %d", &image_w, &image_h) == 2) {
            continue;
        }
        if (sscanf(line,
                   "BLOCKS META %15s %d %d %d %d %d",
                   layout_name,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            if (block_count < 0 || block_count > (int)T23_BORDER_BLOCK_COUNT_MAX) {
                ESP_LOGW(TAG, "BLOCKS META invalid count: %d", block_count);
                return ESP_FAIL;
            }
            g_current_layout = find_layout_desc(layout_name);
            continue;
        }
        if (sscanf(line, "BLOCKS RECT %d %d %d %d %d", &rect_left, &rect_top, &rect_right, &rect_bottom, &thickness) == 5) {
            continue;
        }
        if (sscanf(line, "BLOCK %d %d %d %d", &idx, &r, &g, &b) == 4) {
            if (idx >= 0 && idx < block_count) {
                blocks[idx][0] = r;
                blocks[idx][1] = g;
                blocks[idx][2] = b;
                got_blocks[idx] = 1;
            }
            continue;
        }
        if (strcmp(line, "OK BLOCKS GET") == 0) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "BLOCKS GET failed: %s", line);
            return ESP_FAIL;
        }
    }

    used += (size_t)snprintf(json_buf + used,
                             json_buf_size - used,
                             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d,\"imageWidth\":%d,\"imageHeight\":%d,\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"thickness\":%d},\"blocks\":[",
                             layout_name,
                             block_count,
                             top_blocks,
                             right_blocks,
                             bottom_blocks,
                             left_blocks,
                             image_w,
                             image_h,
                             rect_left,
                             rect_top,
                             rect_right,
                             rect_bottom,
                             thickness);

    for (i = 0; i < block_count; ++i) {
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 "%s{\"index\":%d,\"r\":%d,\"g\":%d,\"b\":%d}",
                                 (i == 0) ? "" : ",",
                                 i,
                                 got_blocks[i] ? blocks[i][0] : 0,
                                 got_blocks[i] ? blocks[i][1] : 0,
                                 got_blocks[i] ? blocks[i][2] : 0);
    }
    snprintf(json_buf + used, json_buf_size - used, "]}");
    return ESP_OK;
}

static esp_err_t build_border_blocks_json(char *json_buf,
                                          size_t json_buf_size,
                                          const char *layout_name,
                                          int block_count,
                                          int top_blocks,
                                          int right_blocks,
                                          int bottom_blocks,
                                          int left_blocks,
                                          int image_w,
                                          int image_h,
                                          int rect_left,
                                          int rect_top,
                                          int rect_right,
                                          int rect_bottom,
                                          int thickness,
                                          const int blocks[T23_BORDER_BLOCK_COUNT_MAX][3],
                                          const int got_blocks[T23_BORDER_BLOCK_COUNT_MAX])
{
    size_t used = 0;
    int i;

    used += (size_t)snprintf(json_buf + used,
                             json_buf_size - used,
                             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d,\"imageWidth\":%d,\"imageHeight\":%d,\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"thickness\":%d},\"blocks\":[",
                             layout_name,
                             block_count,
                             top_blocks,
                             right_blocks,
                             bottom_blocks,
                             left_blocks,
                             image_w,
                             image_h,
                             rect_left,
                             rect_top,
                             rect_right,
                             rect_bottom,
                             thickness);

    for (i = 0; i < block_count; ++i) {
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 "%s{\"index\":%d,\"r\":%d,\"g\":%d,\"b\":%d}",
                                 (i == 0) ? "" : ",",
                                 i,
                                 got_blocks[i] ? blocks[i][0] : 0,
                                 got_blocks[i] ? blocks[i][1] : 0,
                                 got_blocks[i] ? blocks[i][2] : 0);
        if (used >= json_buf_size) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (snprintf(json_buf + used, json_buf_size - used, "]}") >= (int)(json_buf_size - used)) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static int copy_latest_border_json(char *json_buf, size_t json_buf_size)
{
    int valid = 0;

    if (xSemaphoreTake(g_bridge_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_latest_border_json_valid) {
            snprintf(json_buf, json_buf_size, "%s", g_latest_border_json);
            valid = 1;
        }
        xSemaphoreGive(g_bridge_lock);
    }

    return valid;
}

static esp_err_t bridge_fetch_frame_locked(uint8_t *jpeg_buf,
                                           size_t jpeg_buf_size,
                                           size_t *jpeg_len_out)
{
    char line[UART_LINE_MAX];
    char layout_name[16] = "16X9";
    int image_w = 640;
    int image_h = 320;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 639;
    int rect_bottom = 319;
    int thickness = 8;
    int top_blocks = g_current_layout->top_blocks;
    int right_blocks = g_current_layout->right_blocks;
    int bottom_blocks = g_current_layout->bottom_blocks;
    int left_blocks = g_current_layout->left_blocks;
    int block_count = top_blocks + right_blocks + bottom_blocks + left_blocks;
    int blocks[T23_BORDER_BLOCK_COUNT_MAX][3];
    int got_blocks[T23_BORDER_BLOCK_COUNT_MAX] = {0};
    int len;
    uint32_t jpeg_len = 0;
    uint32_t received = 0;

    memset(blocks, 0, sizeof(blocks));
    *jpeg_len_out = 0;
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("FRAME"));

    while (1) {
        int idx;
        int r;
        int g;
        int b;

        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "FRAME timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "BLOCKS SIZE %d %d", &image_w, &image_h) == 2) {
            continue;
        }
        if (sscanf(line,
                   "BLOCKS META %15s %d %d %d %d %d",
                   layout_name,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            if (block_count < 0 || block_count > (int)T23_BORDER_BLOCK_COUNT_MAX) {
                ESP_LOGW(TAG, "FRAME invalid block count: %d", block_count);
                return ESP_FAIL;
            }
            g_current_layout = find_layout_desc(layout_name);
            continue;
        }
        if (sscanf(line, "BLOCKS RECT %d %d %d %d %d", &rect_left, &rect_top, &rect_right, &rect_bottom, &thickness) == 5) {
            continue;
        }
        if (sscanf(line, "BLOCK %d %d %d %d", &idx, &r, &g, &b) == 4) {
            if (idx >= 0 && idx < block_count) {
                blocks[idx][0] = r;
                blocks[idx][1] = g;
                blocks[idx][2] = b;
                got_blocks[idx] = 1;
            }
            continue;
        }
        if (strcmp(line, "OK BLOCKS GET") == 0) {
            continue;
        }
        if (sscanf(line, "SNAP OK %" SCNu32, &jpeg_len) == 1) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "FRAME failed: %s", line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "FRAME unexpected UART line: %s", line);
    }

    if (jpeg_len == 0 || jpeg_len > jpeg_buf_size) {
        ESP_LOGW(TAG, "FRAME invalid jpeg length: %" PRIu32, jpeg_len);
        return ESP_FAIL;
    }

    while (received < jpeg_len) {
        t23_c3_frame_t frame;
        esp_err_t ret = spi_receive_frame(&frame, 3000);

        if (ret != ESP_OK) {
            return ret;
        }
        if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
            ESP_LOGW(TAG, "FRAME bad SPI magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
            return ESP_FAIL;
        }
        if (frame.hdr.type == T23_C3_FRAME_TYPE_RESP_JPEG_INFO) {
            continue;
        }
        if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_JPEG_DATA) {
            ESP_LOGW(TAG, "FRAME unexpected SPI type: %u", frame.hdr.type);
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

    if (build_border_blocks_json(g_latest_border_json,
                                 sizeof(g_latest_border_json),
                                 layout_name,
                                 block_count,
                                 top_blocks,
                                 right_blocks,
                                 bottom_blocks,
                                 left_blocks,
                                 image_w,
                                 image_h,
                                 rect_left,
                                 rect_top,
                                 rect_right,
                                 rect_bottom,
                                 thickness,
                                 blocks,
                                 got_blocks) != ESP_OK) {
        g_latest_border_json_valid = 0;
        return ESP_ERR_NO_MEM;
    }

    g_latest_border_json_valid = 1;
    *jpeg_len_out = received;
    return ESP_OK;
}

static esp_err_t bridge_get_calibration(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    int width = 640;
    int height = 320;
    int pts[16] = {0};
    int len;
    int i;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("CAL GET"));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "CAL GET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strncmp(line, "CAL SIZE ", 9) == 0) {
            if (sscanf(line + 9, "%d %d", &width, &height) != 2) {
                return ESP_FAIL;
            }
            continue;
        }
        if (strncmp(line, "CAL POINTS ", 11) == 0) {
            if (sscanf(line + 11,
                       "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                       &pts[0], &pts[1], &pts[2], &pts[3], &pts[4], &pts[5], &pts[6], &pts[7],
                       &pts[8], &pts[9], &pts[10], &pts[11], &pts[12], &pts[13], &pts[14], &pts[15]) != 16) {
                return ESP_FAIL;
            }
            continue;
        }
        if (strcmp(line, "OK CAL GET") == 0) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            return ESP_FAIL;
        }
    }

    snprintf(json_buf, json_buf_size, "{\"ok\":true,\"imageWidth\":%d,\"imageHeight\":%d,\"points\":[", width, height);
    for (i = 0; i < 8; ++i) {
        char tmp[48];

        snprintf(tmp, sizeof(tmp), "%s{\"x\":%d,\"y\":%d}", (i == 0) ? "" : ",", pts[i * 2], pts[i * 2 + 1]);
        strncat(json_buf, tmp, json_buf_size - strlen(json_buf) - 1);
    }
    strncat(json_buf, "]}", json_buf_size - strlen(json_buf) - 1);
    return ESP_OK;
}

static esp_err_t bridge_set_calibration(const char *query, char *json_buf, size_t json_buf_size)
{
    char cmd[256];
    char value[16];
    int iw = 0;
    int ih = 0;
    int pts[16];
    int i;

    memset(pts, 0, sizeof(pts));
    if (httpd_query_key_value(query, "iw", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    iw = atoi(value);
    if (httpd_query_key_value(query, "ih", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ih = atoi(value);
    for (i = 0; i < 8; ++i) {
        char keyx[8];
        char keyy[8];

        snprintf(keyx, sizeof(keyx), "x%d", i);
        snprintf(keyy, sizeof(keyy), "y%d", i);
        if (httpd_query_key_value(query, keyx, value, sizeof(value)) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        pts[i * 2] = atoi(value);
        if (httpd_query_key_value(query, keyy, value, sizeof(value)) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        pts[i * 2 + 1] = atoi(value);
    }

    snprintf(cmd,
             sizeof(cmd),
             "CAL SET %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
             iw, ih,
             pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], pts[6], pts[7],
             pts[8], pts[9], pts[10], pts[11], pts[12], pts[13], pts[14], pts[15]);

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        int len = uart_read_line(value, sizeof(value), 1500);

        if (len <= 0) {
            ESP_LOGW(TAG, "CAL SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(value, "OK CAL SET") == 0) {
            break;
        }
        if (strncmp(value, "ERR ", 4) == 0) {
            return ESP_FAIL;
        }
    }

    return bridge_get_calibration(json_buf, json_buf_size);
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

static esp_err_t bridge_fetch_snapshot_locked_ex(const char *command,
                                                 const char *ok_prefix,
                                                 uint8_t *jpeg_buf,
                                                 size_t jpeg_buf_size,
                                                 size_t *jpeg_len_out)
{
    char line[UART_LINE_MAX];
    int len;
    uint32_t jpeg_len = 0;
    uint32_t received = 0;

    *jpeg_len_out = 0;
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line(command));

    while (1) {
        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "%s timeout waiting for UART response", command);
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, ok_prefix, &jpeg_len) == 1) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "%s failed: %s", command, line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "%s unexpected UART line: %s", command, line);
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

static esp_err_t bridge_fetch_snapshot_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("SNAP", "SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
}

static esp_err_t bridge_fetch_rectified_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("CAL SNAP", "CAL SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
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

static void runtime_blocks_task(void *arg)
{
    (void)arg;

    while (1) {
        if (g_c3_mode == C3_MODE_RUN) {
            if (xSemaphoreTake(g_bridge_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (bridge_get_border_blocks(g_latest_border_json, sizeof(g_latest_border_json)) == ESP_OK) {
                    g_latest_border_json_valid = 1;
                } else {
                    g_latest_border_json_valid = 0;
                }
                xSemaphoreGive(g_bridge_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static esp_err_t bridge_stream_snapshot(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    ret = bridge_fetch_frame_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK) {
        g_latest_border_json_valid = 0;
        ret = bridge_fetch_snapshot_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    }
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
}

static esp_err_t bridge_stream_rectified(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    ret = bridge_fetch_rectified_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
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

static esp_err_t mode_get_handler(httpd_req_t *req)
{
    char json[64];

    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", c3_mode_name(g_c3_mode));
    return send_json(req, json);
}

static esp_err_t mode_set_handler(httpd_req_t *req)
{
    char query[64];
    char value[16];
    char json[64];
    c3_mode_t target;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing mode query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing mode value", HTTPD_400_BAD_REQUEST);
    }

    if (strcmp(value, "RUN") == 0) {
        target = C3_MODE_RUN;
    } else if (strcmp(value, "DEBUG") == 0) {
        target = C3_MODE_DEBUG;
    } else {
        return send_error_json(req, "unknown mode", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_mode(target, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "mode set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t layout_get_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[128];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_layout(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "layout get failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t layout_set_handler(httpd_req_t *req)
{
    char query[64];
    char value[16];
    char json[128];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing layout query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing layout value", HTTPD_400_BAD_REQUEST);
    }
    if (strcmp(value, "16X9") != 0 && strcmp(value, "4X3") != 0) {
        return send_error_json(req, "unknown layout", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_layout(value, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "layout set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
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
    g_latest_border_json_valid = 0;

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

static esp_err_t calibration_get_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[512];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_calibration(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration get failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t calibration_set_handler(httpd_req_t *req)
{
    char query[256];
    char json[512];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing calibration query", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_calibration(query, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t calibration_rectified_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_rectified(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration rectified failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t border_blocks_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[4096];

    if (copy_latest_border_json(json, sizeof(json))) {
        return send_json(req, json);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_border_blocks(json, sizeof(json));
    if (ret == ESP_OK) {
        snprintf(g_latest_border_json, sizeof(g_latest_border_json), "%s", json);
        g_latest_border_json_valid = 1;
    } else {
        g_latest_border_json_valid = 0;
    }
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "border blocks failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t runtime_blocks_handler(httpd_req_t *req)
{
    char json[4096];
    esp_err_t ret;

    if (!copy_latest_border_json(json, sizeof(json))) {
        xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
        ret = bridge_get_border_blocks(json, sizeof(json));
        if (ret == ESP_OK) {
            snprintf(g_latest_border_json, sizeof(g_latest_border_json), "%s", json);
            g_latest_border_json_valid = 1;
        } else {
            g_latest_border_json_valid = 0;
        }
        xSemaphoreGive(g_bridge_lock);

        if (ret != ESP_OK) {
            return send_error_json(req, "runtime blocks not ready", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    return send_json(req, json);
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
    httpd_uri_t layout_get = {
        .uri = "/api/layout",
        .method = HTTP_GET,
        .handler = layout_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t layout_set = {
        .uri = "/api/layout/set",
        .method = HTTP_GET,
        .handler = layout_set_handler,
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
    httpd_uri_t mode_get = {
        .uri = "/api/mode",
        .method = HTTP_GET,
        .handler = mode_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t mode_set = {
        .uri = "/api/mode/set",
        .method = HTTP_GET,
        .handler = mode_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_get = {
        .uri = "/api/calibration",
        .method = HTTP_GET,
        .handler = calibration_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_set = {
        .uri = "/api/calibration/set",
        .method = HTTP_GET,
        .handler = calibration_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_rectified = {
        .uri = "/api/calibration/rectified",
        .method = HTTP_GET,
        .handler = calibration_rectified_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t border_blocks = {
        .uri = "/api/border_blocks",
        .method = HTTP_GET,
        .handler = border_blocks_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t runtime_blocks = {
        .uri = "/api/runtime_blocks",
        .method = HTTP_GET,
        .handler = runtime_blocks_handler,
        .user_ctx = NULL,
    };
    config.stack_size = 10240;
    config.max_uri_handlers = 16;

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &app_js));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &styles));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ping));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snap));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_rectified));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &border_blocks));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &runtime_blocks));

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
    c3_mode_t initial_mode = C3_MODE_DEBUG;

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
    if (bridge_get_mode(&initial_mode) == ESP_OK) {
        g_c3_mode = initial_mode;
        ESP_LOGI(TAG, "Initial T23 mode: %s", c3_mode_name(g_c3_mode));
    } else {
        ESP_LOGW(TAG, "Failed to query initial T23 mode, defaulting to DEBUG");
        g_c3_mode = C3_MODE_DEBUG;
    }
    start_http_server();
    xTaskCreate(runtime_blocks_task, "runtime_blocks", 12288, NULL, 5, NULL);
}
