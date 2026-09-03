#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "camera.h"
#include "esp_camera.h"
#include "ble.h"
#include "CNN.h"

static const char *TAG = "SERVER";

// ─── NVS namespace / keys ────────────────────────────────────────────────────
#define NVS_NAMESPACE   "wifi_mgr"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

// ─── AP (setup portal) settings ──────────────────────────────────────────────
#define AP_SSID         "BabyMonitor-Setup"
#define AP_PASS         ""          // open network; set a password if desired
#define AP_CHANNEL      1
#define AP_MAX_CONN     4

// ─── STA settings ────────────────────────────────────────────────────────────
#define WIFI_MAX_RETRY  10

// ─── Event group bits ────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  NVS helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool nvs_load_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz) == ESP_OK) &&
              (nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz) == ESP_OK) &&
              ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
}

static void nvs_erase_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials erased");
}

// ─────────────────────────────────────────────────────────────────────────────
//  URL-decode helper (for form POST body)
// ─────────────────────────────────────────────────────────────────────────────
static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

// Extract key=value from a query/POST string.  Returns true on success.
static bool extract_param(const char *body, const char *key, char *out, size_t out_sz)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    char encoded[256] = {0};
    strncpy(encoded, p, len);
    url_decode(encoded, out, out_sz);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Captive-portal / setup handlers  (AP mode, port 80)
// ─────────────────────────────────────────────────────────────────────────────
static const char SETUP_HTML[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
    "<title>Little Guardian &#x2014; WiFi Setup</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',sans-serif;"
    "     background:linear-gradient(135deg,#64b5f6,#1565c0);"
    "     min-height:100vh;display:flex;justify-content:center;align-items:center;padding:20px}"
    ".card{background:#fff;border-radius:20px;padding:40px;max-width:420px;"
    "      width:100%%;box-shadow:0 20px 60px rgba(0,0,0,.3);text-align:center}"
    "h1{color:#333;margin-bottom:8px}p{color:#777;margin-bottom:24px;font-size:.9em}"
    "label{display:block;text-align:left;color:#555;font-size:.85em;margin-bottom:4px}"
    "input{width:100%%;padding:12px;border:1px solid #ddd;border-radius:8px;"
    "      font-size:1em;margin-bottom:16px;outline:none}"
    "input:focus{border-color:#64b5f6}"
    "button{width:100%%;padding:14px;background:linear-gradient(135deg,#64b5f6,#1565c0);"
    "       color:#fff;border:none;border-radius:10px;font-size:1em;cursor:pointer}"
    "button:hover{opacity:.9}"
    ".msg{margin-top:16px;font-size:.85em;color:#e53935}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>Little Guardian</h1>"
    "<p>Enter Your 2.4 GHz WiFi Credentials<br>To Connect The Device To Your Network.</p>"
    "<form method='POST' action='/save'>"
    "<label>Network Name (SSID)</label>"
    "<input name='ssid' type='text' placeholder='MyHomeWiFi' required>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='Password'>"
    "<button type='submit'>Connect</button>"
    "</form>"
    "%s"   /* optional status message */
    "</div></body></html>";

static httpd_handle_t s_setup_server  = NULL;
static httpd_handle_t s_main_server80 = NULL;
static httpd_handle_t s_main_server81 = NULL;

// Forward declarations
static void start_setup_server(void);
static void start_main_servers(void);
static void stop_setup_server(void);

// ── GET /  (setup portal) ─────────────────────────────────────────────────────
static esp_err_t setup_root_handler(httpd_req_t *req)
{
    char html[sizeof(SETUP_HTML) + 64];
    snprintf(html, sizeof(html), SETUP_HTML, "");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ── POST /save  (setup portal) ────────────────────────────────────────────────
static esp_err_t setup_save_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    char ssid[64] = {0}, pass[64] = {0};
    if (!extract_param(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        char html[sizeof(SETUP_HTML) + 128];
        snprintf(html, sizeof(html), SETUP_HTML,
                 "<div class='msg'>&#x26A0; SSID Cannot Be Empty.</div>");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    }
    extract_param(body, "pass", pass, sizeof(pass));

    const char *ack =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:'Segoe UI',sans-serif;"
        "background:linear-gradient(135deg,#64b5f6,#1565c0);"
        "display:flex;justify-content:center;align-items:center;min-height:100vh}"
        ".c{background:#fff;border-radius:20px;padding:40px;text-align:center;"
        "max-width:400px;box-shadow:0 20px 60px rgba(0,0,0,.3)}"
        "h2{color:#333;margin-bottom:12px}p{color:#666;font-size:.9em}</style></head>"
        "<body><div class='c'><h2>&#x2705; Credentials Saved</h2>"
        "<p>The Device Will Now Attempt To Connect To Your Network.<br><br>"
        "If Successful, It Will Be Available At<br>"
        "<strong>http://littleguardian.local</strong><br><br>"
        "This Access Point Will Disappear. Close This Page.</p></div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ack, HTTPD_RESP_USE_STRLEN);

    nvs_save_wifi(ssid, pass);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// Captive-portal redirect: any unknown URL → /
static esp_err_t setup_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static const httpd_uri_t setup_root_uri = { .uri="/",     .method=HTTP_GET,  .handler=setup_root_handler, .user_ctx=NULL };
static const httpd_uri_t setup_save_uri = { .uri="/save", .method=HTTP_POST, .handler=setup_save_handler, .user_ctx=NULL };

static void start_setup_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.max_open_sockets = 5;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 8192;

    if (httpd_start(&s_setup_server, &cfg) == ESP_OK) {
        httpd_register_uri_handler(s_setup_server, &setup_root_uri);
        httpd_register_uri_handler(s_setup_server, &setup_save_uri);
        httpd_register_err_handler(s_setup_server, HTTPD_404_NOT_FOUND,
            setup_redirect_handler);
        ESP_LOGI(TAG, "Setup portal started at http://192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Failed to start setup portal");
    }
}

static void stop_setup_server(void)
{
    if (s_setup_server) {
        httpd_stop(s_setup_server);
        s_setup_server = NULL;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main app HTTP handlers  (STA mode)
// ─────────────────────────────────────────────────────────────────────────────

// ── MJPEG stream (port 81) ────────────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    ESP_LOGI(TAG, "MJPEG stream started");

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        uint8_t *jpg_buf = NULL;
        size_t   jpg_len = 0;
        bool ok = frame2jpg(fb, 30, &jpg_buf, &jpg_len);
        esp_camera_fb_return(fb);
        if (!ok) { free(jpg_buf); continue; }

        char header[128];
        int hlen = snprintf(header, sizeof(header),
            "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned)jpg_len);

        bool sent = (httpd_resp_send_chunk(req, header, hlen) == ESP_OK) &&
                    (httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len) == ESP_OK);
        free(jpg_buf);
        if (!sent) { ESP_LOGI(TAG, "Client disconnected from stream"); break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Stream ended");
    return ESP_OK;
}

// ── /data — JSON polled by JS (port 80) ───────────────────────────────────────
static esp_err_t data_handler(httpd_req_t *req)
{
    int   heart = 0, o2 = 0, battery = 0;
    float temp  = 0.0f;
    if (ble_data_ready) {
        heart   = received_data.heart_rate;
        o2      = received_data.o2;
        battery = received_data.battery;
        temp    = received_data.body_temp;
    }
    cnn_prediction_t pred = cnn_get_last_prediction();

    char json[512];
    snprintf(json, sizeof(json),
        "{"
          "\"heart\":%d,"
          "\"o2\":%d,"
          "\"battery\":%d,"
          "\"temp\":%.1f,"
          "\"position\":\"%s\","
          "\"confidence\":%.1f,"
          "\"prob_back\":%.1f,"
          "\"prob_stomach\":%.1f,"
          "\"prob_side\":%.1f"
        "}",
        heart, o2, battery, temp,
        pred.class_name,
        pred.confidence * 100.0f,
        pred.prob[0] * 100.0f,
        pred.prob[1] * 100.0f,
        pred.prob[2] * 100.0f);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// ── POST /forget — erase NVS creds and reboot into AP mode ────────────────────
static esp_err_t forget_handler(httpd_req_t *req)
{
    const char *resp =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<style>body{font-family:'Segoe UI',sans-serif;"
        "background:linear-gradient(135deg,#64b5f6,#1565c0);"
        "display:flex;justify-content:center;align-items:center;min-height:100vh}"
        ".c{background:#fff;border-radius:20px;padding:40px;text-align:center;"
        "max-width:400px;box-shadow:0 20px 60px rgba(0,0,0,.3)}"
        "h2{color:#333;margin-bottom:12px}p{color:#666;font-size:.9em}</style></head>"
        "<body><div class='c'><h2>WiFi Credentials Erased.</h2>"
        "<p>The Device Is Rebooting Into Setup Mode.<br>"
        "Connect To <strong>BabyMonitor-Setup</strong> To Reconfigure.</p>"
        "</div></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_erase_wifi();
    esp_restart();
    return ESP_OK;
}

// ── Root page (port 80) ────────────────────────────────────────────────────────
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "root_get_handler called");
    int   heart = 0, o2 = 0, battery = 0;
    float temp  = 0.0f;
    if (ble_data_ready) {
        heart   = received_data.heart_rate;
        o2      = received_data.o2;
        battery = received_data.battery;
        temp    = received_data.body_temp;
    }
    cnn_prediction_t pred = cnn_get_last_prediction();

    char host_buf[64] = "littleguardian.local";
    if (httpd_req_get_hdr_value_str(req, "Host", host_buf, sizeof(host_buf)) == ESP_OK) {
        char *colon = strchr(host_buf, ':');
        if (colon) *colon = '\0';
    }

    static char html[16384];
    int written = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
        "<title>Little Guardian</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:'Segoe UI',sans-serif;"
        "     background:linear-gradient(135deg,#64b5f6 0%%,#1565c0 100%%);"
        "     min-height:100vh;display:flex;justify-content:center;"
        "     align-items:center;padding:20px}"
        ".container{background:#fff;border-radius:20px;"
        "           box-shadow:0 20px 60px rgba(0,0,0,.3);"
        "           padding:40px;max-width:900px;width:100%%;text-align:center}"
        "h1{color:#333;margin-bottom:10px;font-size:2.8em}"
        ".subtitle{color:#666;margin-bottom:30px;font-size:.9em}"
        ".cam{background:#f0f7ff;border-radius:15px;padding:20px;margin-top:20px}"
        ".cam h2{color:#555;margin-bottom:15px;font-size:1.2em}"
        ".cam img{border:3px solid #64b5f6;border-radius:12px;"
        "         max-width:100%%;height:auto;box-shadow:0 4px 15px rgba(0,0,0,.1)}"
        ".status{margin-top:15px;font-size:.9em;color:#555}"
        ".pred{background:#e3f2fd;border-radius:15px;padding:25px;margin-top:25px;"
        "      box-shadow:0 4px 10px rgba(0,0,0,.1)}"
        ".pred h2{color:#0d47a1;margin-bottom:18px;font-size:1.2em}"
        ".plabel{font-size:1.8em;font-weight:bold;color:#1565c0;margin-bottom:6px}"
        ".pconf{font-size:1em;color:#1976d2;margin-bottom:18px}"
        ".brow{display:flex;align-items:center;margin:8px 0;font-size:.9em}"
        ".bname{width:80px;text-align:right;padding-right:10px;color:#555;font-weight:bold}"
        ".btrack{flex:1;background:#bbdefb;border-radius:6px;height:20px;overflow:hidden}"
        ".bfill{height:100%%;background:linear-gradient(90deg,#64b5f6,#1565c0);"
        "       border-radius:6px;transition:width .6s ease}"
        ".bpct{width:48px;text-align:left;padding-left:8px;color:#555}"
        ".vband{background:#e3f2fd;border-radius:15px;padding:25px;margin-top:25px;"
        "       box-shadow:0 4px 10px rgba(0,0,0,.1)}"
        ".vband h2{color:#0d47a1;margin-bottom:18px;font-size:1.2em}"
        ".ts{margin-top:10px;font-size:.75em;color:#aaa}"
        ".cards{display:flex;flex-wrap:wrap;justify-content:space-around;margin-top:25px}"
        ".card{flex:1 1 40%%;min-width:150px;background:#e3f2fd;border-radius:12px;"
        "      padding:20px;margin:10px;font-size:1.3em;color:#1565c0;"
        "      font-weight:bold;box-shadow:0 4px 10px rgba(0,0,0,.1);"
        "      transition:transform .2s ease,box-shadow .2s ease;cursor:default}"
        ".card:hover{transform:translateY(-4px);box-shadow:0 10px 24px rgba(0,0,0,.15)}"
        ".card span{display:block;font-size:.8em;color:#555;margin-top:5px;font-weight:normal}"
        /* ── Alarm panel styles ── */
        ".alarm-panel{display:none;border-radius:15px;padding:25px;margin-top:25px;"
"             box-shadow:0 4px 10px rgba(0,0,0,.1);transition:background .4s ease}"
        ".alarm-panel.safe{background:#e8f5e9}"
        ".alarm-panel.danger{background:#ffebee;animation:pulse-bg 1s infinite alternate}"
        "@keyframes pulse-bg{from{background:#ffebee}to{background:#ffcdd2}}"
        ".alarm-panel h2{margin-bottom:14px;font-size:1.2em}"
        ".alarm-panel.safe h2{color:#2e7d32}"
        ".alarm-panel.danger h2{color:#c62828}"
        "#alarmStatus{font-size:1.05em;margin-bottom:14px;font-weight:600}"
        ".alarm-panel.safe #alarmStatus{color:#388e3c}"
        ".alarm-panel.danger #alarmStatus{color:#c62828}"
        "#silenceBtn{padding:12px 28px;"
        "            background:linear-gradient(135deg,#ef5350,#b71c1c);"
        "            color:#fff;border:none;border-radius:10px;"
        "            font-size:1em;cursor:pointer;display:none;"
        "            box-shadow:0 4px 12px rgba(183,28,28,.4);"
        "            transition:opacity .2s ease}"
        "#silenceBtn:hover{opacity:.85}"
        "</style></head><body>"
        "<div class='container'>"
        "<h1>Little Guardian</h1>"

        /* ── Camera ── */
        "<div class='cam'>"
        "<h2>Live Feed</h2>"
        "<img src='http://%s:81/stream' width='640' alt='Camera Stream'>"
        "</div>"

        /* ── Sleep position ── */
        "<div class='pred'>"
        "<h2>Sleep Position</h2>"
        "<div class='plabel' id='vpos'>%s</div>"
        "<div class='pconf'  id='vcnf'>Confidence: %.1f%%</div>"
        "<div class='brow'><div class='bname'>Back</div>"
        "  <div class='btrack'><div class='bfill' id='bb' style='width:%.0f%%'></div></div>"
        "  <div class='bpct' id='pb'>%.0f%%</div></div>"
        "<div class='brow'><div class='bname'>Stomach</div>"
        "  <div class='btrack'><div class='bfill' id='bs' style='width:%.0f%%'></div></div>"
        "  <div class='bpct' id='ps'>%.0f%%</div></div>"
        "<div class='brow'><div class='bname'>Side</div>"
        "  <div class='btrack'><div class='bfill' id='bsd' style='width:%.0f%%'></div></div>"
        "  <div class='bpct' id='psd'>%.0f%%</div></div>"
        "<div class='ts' id='ts'>Waiting For First Update...</div>"
        "</div>"

        /* ── Alarm panel ── */
        "<div class='alarm-panel' id='alarmPanel' style='display:none;'>"
        "<h2>&#x26A0; Stomach Alert</h2>"
        "<div id='alarmStatus'>Monitoring...</div>"
        "<button id='silenceBtn' onclick='silenceAlarm()'>&#x1F514; Silence Alarm</button>"
        "</div>"

        /* ── Vital band ── */
        "<div class='vband'>"
        "<h2>Vital Band</h2>"
        "<div class='cards'>"
        "<div class='card'>Heart Rate<br><span id='vhr'>%d bpm</span></div>"
        "<div class='card'>O2<br><span id='vo2'>%d%%</span></div>"
        "<div class='card'>Battery<br><span id='vbt'>%d%%</span></div>"
        "<div class='card'>Skin Temperature<br><span id='vtm'>%.1f&deg;C</span></div>"
        "</div>"
        "</div>"

        "</div>"  /* /container */

        "<script>"
        /* ── Alarm state ── */
        "var alarmCtx=null,alarmNode=null,alarmActive=false,silenced=false;"

        "function startAlarm(){"
        "  if(alarmActive||silenced)return;"
        "  alarmActive=true;"
        "  var p=document.getElementById('alarmPanel');"
        "  p.className='alarm-panel danger';"
        "  document.getElementById('alarmStatus').textContent='\u26a0\ufe0f STOMACH POSITION DETECTED!';"
        "  document.getElementById('silenceBtn').style.display='inline-block';"
        "document.getElementById('alarmPanel').style.display = 'block';"
        "  alarmCtx=new(window.AudioContext||window.webkitAudioContext)();"
        "  function beep(){"
        "    if(!alarmActive)return;"
        "    var o=alarmCtx.createOscillator();"
        "    var g=alarmCtx.createGain();"
        "    o.connect(g);g.connect(alarmCtx.destination);"
        "    o.type='square';"
        "    o.frequency.setValueAtTime(880,alarmCtx.currentTime);"
        "    o.frequency.setValueAtTime(660,alarmCtx.currentTime+0.2);"
        "    g.gain.setValueAtTime(0.4,alarmCtx.currentTime);"
        "    g.gain.exponentialRampToValueAtTime(0.001,alarmCtx.currentTime+0.4);"
        "    o.start(alarmCtx.currentTime);"
        "    o.stop(alarmCtx.currentTime+0.4);"
        "    alarmNode=setTimeout(beep,500);"
        "  }"
        "  beep();"
        "}"

        "function stopAlarm(){"
        "  alarmActive=false;"
        "  clearTimeout(alarmNode);"
        "  if(alarmCtx){alarmCtx.close();alarmCtx=null;}"
        "  var p=document.getElementById('alarmPanel');"
        "  p.className='alarm-panel safe';"
        "  document.getElementById('alarmStatus').textContent='Monitoring...';"
        "  document.getElementById('silenceBtn').style.display='none';"
        "document.getElementById('alarmPanel').style.display = 'none';"
        "}"

        "function silenceAlarm(){"
        "  silenced=true;"
        "  stopAlarm();"
        "  document.getElementById('alarmStatus').textContent='Alarm silenced \u2014 still monitoring';"
        "}"

        /* ── Poll & update ── */
        "function upd(d){"
        "  document.getElementById('vpos').textContent=d.position;"
        "  document.getElementById('vcnf').textContent='Confidence: '+d.confidence.toFixed(1)+'%%';"
        "  document.getElementById('bb').style.width=d.prob_back+'%%';"
        "  document.getElementById('pb').textContent=d.prob_back.toFixed(0)+'%%';"
        "  document.getElementById('bs').style.width=d.prob_stomach+'%%';"
        "  document.getElementById('ps').textContent=d.prob_stomach.toFixed(0)+'%%';"
        "  document.getElementById('bsd').style.width=d.prob_side+'%%';"
        "  document.getElementById('psd').textContent=d.prob_side.toFixed(0)+'%%';"
        "  document.getElementById('vhr').textContent=d.heart+' bpm';"
        "  document.getElementById('vo2').textContent=d.o2+'%%';"
        "  document.getElementById('vbt').textContent=d.battery+'%%';"
        "  document.getElementById('vtm').textContent=d.temp.toFixed(1)+'\u00b0C';"
        "  document.getElementById('ts').textContent='Last Updated: '+new Date().toLocaleTimeString();"
        /* Trigger alarm only when stomach AND confidence > 70% */
        "  if(d.position==='Stomach'&&d.confidence>70){"
        "    silenced=false;"  /* re-arm after baby moves away and returns */
        "    startAlarm();"
        "  }else{"
        "    stopAlarm();"
        "  }"
        "}"

        "function poll(){"
        "  fetch('/data?t='+Date.now())"
        "    .then(r=>r.json()).then(upd)"
        "    .catch(e=>console.warn(e))"
        "    .finally(()=>setTimeout(poll,5000));"
        "}"
        "poll();"
        "</script>"
        "</body></html>",

        host_buf,
        pred.class_name, pred.confidence * 100.0f,
        pred.prob[0]*100.0f, pred.prob[0]*100.0f,
        pred.prob[1]*100.0f, pred.prob[1]*100.0f,
        pred.prob[2]*100.0f, pred.prob[2]*100.0f,
        heart, o2, battery, temp
    );

    if (written >= (int)sizeof(html))
        ESP_LOGE(TAG, "HTML buffer truncated! written=%d", written);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "404 Not Found");
    return ESP_FAIL;
}

static const httpd_uri_t root_uri   = { .uri="/",       .method=HTTP_GET,  .handler=root_get_handler, .user_ctx=NULL };
static const httpd_uri_t stream_uri = { .uri="/stream", .method=HTTP_GET,  .handler=stream_handler,   .user_ctx=NULL };
static const httpd_uri_t data_uri   = { .uri="/data",   .method=HTTP_GET,  .handler=data_handler,     .user_ctx=NULL };
static const httpd_uri_t forget_uri = { .uri="/forget", .method=HTTP_POST, .handler=forget_handler,   .user_ctx=NULL };

static void start_main_servers(void)
{
    // Port 80: page + data + forget
    httpd_config_t cfg80 = HTTPD_DEFAULT_CONFIG();
    cfg80.server_port       = 80;
    cfg80.ctrl_port         = 32768;
    cfg80.max_open_sockets  = 5;
    cfg80.max_uri_handlers  = 8;
    cfg80.max_resp_headers  = 8;
    cfg80.backlog_conn      = 5;
    cfg80.lru_purge_enable  = true;
    cfg80.recv_wait_timeout = 5;
    cfg80.send_wait_timeout = 5;
    cfg80.stack_size        = 16384;

    if (httpd_start(&s_main_server80, &cfg80) == ESP_OK) {
        httpd_register_uri_handler(s_main_server80, &root_uri);
        httpd_register_uri_handler(s_main_server80, &data_uri);
        httpd_register_uri_handler(s_main_server80, &forget_uri);
        httpd_register_err_handler(s_main_server80, HTTPD_404_NOT_FOUND, http_404_error_handler);
        ESP_LOGI(TAG, "Port 80 started (main app)");
    } else {
        ESP_LOGE(TAG, "Failed to start port 80");
    }

    // Port 81: MJPEG stream
    httpd_config_t cfg81 = HTTPD_DEFAULT_CONFIG();
    cfg81.server_port       = 81;
    cfg81.ctrl_port         = 32769;
    cfg81.max_open_sockets  = 3;
    cfg81.max_uri_handlers  = 4;
    cfg81.max_resp_headers  = 4;
    cfg81.backlog_conn      = 2;
    cfg81.lru_purge_enable  = true;
    cfg81.recv_wait_timeout = 5;
    cfg81.send_wait_timeout = 5;
    cfg81.stack_size        = 8192;

    if (httpd_start(&s_main_server81, &cfg81) == ESP_OK) {
        httpd_register_uri_handler(s_main_server81, &stream_uri);
        ESP_LOGI(TAG, "Port 81 started (MJPEG stream)");
    } else {
        ESP_LOGE(TAG, "Failed to start port 81");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi event handler
// ─────────────────────────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "STA connection failed after %d retries — erasing creds and rebooting to AP mode", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public entry point:  wifi_manager_start()
// ─────────────────────────────────────────────────────────────────────────────
void wifi_manager_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[64] = {0}, pass[64] = {0};
    bool has_creds = nvs_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    if (!has_creds) {
        // ── AP / setup mode ──────────────────────────────────────────────────
        ESP_LOGI(TAG, "No WiFi credentials found — starting setup AP '%s'", AP_SSID);

        esp_netif_create_default_wifi_ap();
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

        wifi_config_t ap_cfg = {
            .ap = {
                .ssid           = AP_SSID,
                .ssid_len       = strlen(AP_SSID),
                .channel        = AP_CHANNEL,
                .password       = AP_PASS,
                .max_connection = AP_MAX_CONN,
                .authmode       = (strlen(AP_PASS) == 0)
                                    ? WIFI_AUTH_OPEN
                                    : WIFI_AUTH_WPA2_PSK,
            }
        };
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "AP started — connect to '%s' then visit http://192.168.4.1", AP_SSID);
        start_setup_server();

    } else {
        // ── STA mode ─────────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Credentials found — connecting STA to '%s'", ssid);

        s_wifi_event_group = xEventGroupCreate();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_ERROR_CHECK(mdns_init());
            mdns_hostname_set("littleguardian");
            mdns_instance_name_set("Baby Sleep Monitor");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "Ready at http://littleguardian.local");
            start_main_servers();
        } else {
            ESP_LOGE(TAG, "STA connection failed — clearing creds, rebooting to setup AP");
            nvs_erase_wifi();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

void wifi_manager_stop(void)
{
    stop_setup_server();
    if (s_main_server80) { httpd_stop(s_main_server80); s_main_server80 = NULL; }
    if (s_main_server81) { httpd_stop(s_main_server81); s_main_server81 = NULL; }
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi manager stopped");
}