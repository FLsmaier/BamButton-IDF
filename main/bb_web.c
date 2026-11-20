#include "bb_web.h"

#include "bb_api.h"
#include "bb_nvs.h"
#include "bb_ota.h"
#include "bb_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bb.web";

static bb_config_t *s_cfg;
static bb_runtime_fn s_runtime;
static SemaphoreHandle_t s_lock;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

typedef enum {
    FORM_MISSING,
    FORM_OK,
    FORM_TOO_LONG,
} form_status_t;

static form_status_t decode_span(const char *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t used = 0;
    size_t i = 0;
    if (dst == NULL || dst_len == 0) {
        return FORM_TOO_LONG;
    }
    while (i < src_len) {
        unsigned char ch;
        if (src[i] == '+') {
            ch = ' ';
            i++;
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_nibble(src[i + 1]);
            int lo = hex_nibble(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                ch = (unsigned char)((hi << 4) | lo);
                i += 3;
            } else {
                ch = (unsigned char)src[i++];
            }
        } else {
            ch = (unsigned char)src[i++];
        }
        if (used + 1 >= dst_len || ch == 0) {
            dst[0] = '\0';
            return FORM_TOO_LONG;
        }
        dst[used++] = (char)ch;
    }
    dst[used] = '\0';
    return FORM_OK;
}

static form_status_t form_value(const char *body, const char *key, char *out, size_t out_len)
{
    const char *p = body != NULL ? body : "";
    if (out == NULL || out_len == 0 || key == NULL) {
        return FORM_TOO_LONG;
    }
    out[0] = '\0';
    form_status_t found = FORM_MISSING;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t n = amp != NULL ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', n);
        if (eq != NULL) {
            char key_buf[32];
            size_t key_len = (size_t)(eq - p);
            if (decode_span(p, key_len, key_buf, sizeof(key_buf)) == FORM_OK && strcmp(key_buf, key) == 0) {
                form_status_t value = decode_span(eq + 1, n - key_len - 1, out, out_len);
                if (value != FORM_OK) {
                    return FORM_TOO_LONG;
                }
                found = FORM_OK;
            }
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return found;
}

static bool field_fits(form_status_t status, char *err, size_t err_len, const char *too_long)
{
    if (status != FORM_TOO_LONG) {
        return true;
    }
    if (err != NULL && err_len > 0 && too_long != NULL) {
        size_t len = strlen(too_long);
        if (len >= err_len) {
            len = err_len - 1;
        }
        memcpy(err, too_long, len);
        err[len] = '\0';
    }
    return false;
}

static void html_chunk(httpd_req_t *req, char *buf, size_t cap, size_t *used, const char *text, size_t text_len)
{
    if (text_len == 0 || text_len >= cap) {
        return;
    }
    if (*used + text_len >= cap) {
        buf[*used] = '\0';
        httpd_resp_sendstr_chunk(req, buf);
        *used = 0;
    }
    memcpy(buf + *used, text, text_len);
    *used += text_len;
}

static void html_text(httpd_req_t *req, const char *in)
{
    char buf[64];
    size_t used = 0;
    if (in == NULL) {
        return;
    }
    for (; *in != '\0'; in++) {
        const char *rep;
        size_t n;
        char one[2];
        switch (*in) {
        case '&':
            rep = "&amp;";
            n = 5;
            break;
        case '<':
            rep = "&lt;";
            n = 4;
            break;
        case '>':
            rep = "&gt;";
            n = 4;
            break;
        case '"':
            rep = "&quot;";
            n = 6;
            break;
        case '\'':
            rep = "&#39;";
            n = 5;
            break;
        default:
            one[0] = *in;
            rep = one;
            n = 1;
            break;
        }
        html_chunk(req, buf, sizeof(buf), &used, rep, n);
    }
    if (used > 0) {
        buf[used] = '\0';
        httpd_resp_sendstr_chunk(req, buf);
    }
}

static esp_err_t begin_html(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                                          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                                          "<title>BamButton-IDF</title><style>"
                                          "html{color-scheme:light;background:#f4f4f5;color:#18181b}"
                                          "body{font-family:system-ui,sans-serif;max-width:40rem;margin:0 auto;"
                                          "padding:1.1rem 1.2rem 2.2rem;line-height:1.45}"
                                          "h1{font-size:1.35rem;margin:0 0 .85rem}"
                                          ".status{display:grid;grid-template-columns:1fr 1fr;"
                                          "gap:.5rem;margin:0 0 .7rem}"
                                          ".status div{background:#fff;border:1px solid #e4e4e7;border-radius:.55rem;"
                                          "padding:.5rem .7rem;min-width:0}"
                                          ".status span{display:block;color:#71717a;font-size:.68rem;font-weight:600;"
                                          "letter-spacing:.04em;text-transform:uppercase}"
                                          ".status strong{display:block;margin-top:.12rem;font-size:.95rem;font-weight:600;"
                                          "overflow-wrap:anywhere}"
                                          ".status em{display:block;margin-top:.15rem;font-style:normal;font-size:.82rem;"
                                          "font-weight:500;color:#3f3f46}.status em:empty{display:none}"
                                          ".note{margin:.1rem 0 .9rem;color:#52525b;font-size:.9rem}.note:empty{display:none}"
                                          "fieldset{background:#fff;border:1px solid #e4e4e7;border-radius:.6rem;"
                                          "margin:0 0 .75rem;padding:.3rem .9rem .85rem}"
                                          "fieldset hr{border:0;border-top:1px solid #e4e4e7;margin:.9rem 0 .2rem}"
                                          "legend{font-weight:600;padding:0 .25rem}"
                                          "label{display:block;margin:.6rem 0;font-size:.9rem}"
                                          "input,select{display:block;width:100%;box-sizing:border-box;margin-top:.28rem;"
                                          "padding:.48rem .6rem;border:1px solid #d4d4d8;border-radius:.4rem;"
                                          "background:#fff;color:#18181b;font:inherit}"
                                          "label.check{display:flex;align-items:center;gap:.45rem}"
                                          "label.check input{width:auto;margin:0;padding:0}"
                                          ".row{display:grid;grid-template-columns:1fr 1fr;gap:0 .75rem}"
                                          "@media(max-width:34rem){.row{grid-template-columns:1fr}}"
                                          "button{font:inherit;padding:.5rem .85rem;border:0;border-radius:.4rem;"
                                          "background:#18181b;color:#fff;margin:.15rem .35rem .15rem 0}"
                                          "button.ghost{background:#fff;color:#18181b;border:1px solid #d4d4d8}"
                                          "button.danger{background:#b91c1c}"
                                          ".hint{color:#3f3f46;font-size:.9rem}"
                                          ".message{background:#e0e7ff;border-radius:.45rem;padding:.6rem .75rem}"
                                          "fieldset p{margin:.35rem 0 .7rem;color:#3f3f46}"
                                          "a{color:#1d4ed8}ul{padding-left:1.15rem}li{margin:.28rem 0}"
                                          "</style></head><body>");
}

static esp_err_t end_html(httpd_req_t *req)
{
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void send_gpio_select(httpd_req_t *req, const char *label, const char *name, uint8_t selected,
                             uint8_t fallback)
{
    httpd_resp_sendstr_chunk(req, "<label>");
    httpd_resp_sendstr_chunk(req, label);
    httpd_resp_sendstr_chunk(req, " <select name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\">");
    for (size_t i = 0; i < BB_GPIO_CHOICE_COUNT; i++) {
        uint8_t pin = bb_config_gpio_choice(i);
        char option[80];
        int wrote = snprintf(option, sizeof(option), "<option value=\"%u\"%s>GPIO %u%s</option>", pin,
                             pin == selected ? " selected" : "", pin, pin == fallback ? " (default)" : "");
        if (wrote > 0 && (size_t)wrote < sizeof(option)) {
            httpd_resp_sendstr_chunk(req, option);
        } else {
            ESP_LOGE(TAG, "GPIO option did not fit");
        }
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");
}

static void send_config_page(httpd_req_t *req, const bb_config_t *cfg, const char *message, int status)
{
    char station_ssid[33];
    char station_ip[16];
    char station_mask[16];
    char printer[16];
    const esp_app_desc_t *app = esp_app_get_description();

    bb_wifi_get_station(station_ssid, sizeof(station_ssid), station_ip, sizeof(station_ip), station_mask,
                        sizeof(station_mask));
    const bool station_up = bb_wifi_sta_connected() && station_ssid[0] != '\0';
    char signal[32];
    const bool have_signal = station_up && bb_wifi_format_signal(signal, sizeof(signal));
    snprintf(printer, sizeof(printer), "%d", (int)cfg->printer_id);

    httpd_resp_set_status(req, status == 200 ? "200 OK" : "400 Bad Request");
    begin_html(req);
    httpd_resp_sendstr_chunk(req, "<h1>BamButton-IDF</h1><section class=\"status\"><div><span>Setup</span><strong>");
    html_text(req, bb_wifi_hostname());
    httpd_resp_sendstr_chunk(req, "</strong></div><div><span>Station</span><strong id=\"sta-ssid\">");
    if (station_up) {
        html_text(req, station_ssid);
    } else {
        httpd_resp_sendstr_chunk(req, "Not joined");
    }
    httpd_resp_sendstr_chunk(req, "</strong><em id=\"sta-signal\">");
    if (have_signal) {
        html_text(req, signal);
    }
    httpd_resp_sendstr_chunk(req, "</em></div><div id=\"sta-ip-cell\"");
    if (!station_up) {
        httpd_resp_sendstr_chunk(req, " hidden");
    }
    httpd_resp_sendstr_chunk(req, "><span>Address</span><strong id=\"sta-ip\">");
    if (station_up && station_ip[0] != '\0') {
        html_text(req, station_ip);
    } else {
        httpd_resp_sendstr_chunk(req, "&mdash;");
    }
    httpd_resp_sendstr_chunk(req, "</strong></div><div id=\"sta-mask-cell\"");
    if (!station_up) {
        httpd_resp_sendstr_chunk(req, " hidden");
    }
    httpd_resp_sendstr_chunk(req, "><span>Netmask</span><strong id=\"sta-mask\">");
    if (station_up && station_mask[0] != '\0') {
        html_text(req, station_mask);
    } else {
        httpd_resp_sendstr_chunk(req, "&mdash;");
    }
    httpd_resp_sendstr_chunk(req, "</strong></div></section><p class=\"note\" id=\"ap-note\">");
    if (bb_wifi_ap_active() && station_up) {
        httpd_resp_sendstr_chunk(req, "Setup access point is on at 192.168.4.1. It turns off 60 seconds after the station stays joined.");
    } else if (bb_wifi_ap_active()) {
        httpd_resp_sendstr_chunk(req, "Setup access point is on at 192.168.4.1.");
    }
    httpd_resp_sendstr_chunk(req, "</p>");
    if (message != NULL && message[0] != '\0') {
        httpd_resp_sendstr_chunk(req, "<p class=\"message\">");
        html_text(req, message);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/save\"><fieldset><legend>Network</legend>"
                                  "<label>Wi-Fi SSID <input name=\"wifi_ssid\" value=\"");
    html_text(req, cfg->wifi_ssid);
    httpd_resp_sendstr_chunk(req, "\"></label><label>Wi-Fi password <input type=\"password\" name=\"wifi_password\" "
                                  "autocomplete=\"new-password\" placeholder=\"Leave blank to keep current\"></label>"
                                  "<label class=\"check\"><input type=\"checkbox\" name=\"use_dhcp\" value=\"1\"");
    if (cfg->use_dhcp) {
        httpd_resp_sendstr_chunk(req, " checked");
    }
    httpd_resp_sendstr_chunk(req, "> Use DHCP</label><div class=\"row\"><label>IP address <input name=\"ip\" value=\"");
    html_text(req, cfg->ip);
    httpd_resp_sendstr_chunk(req, "\"></label><label>Netmask <input name=\"netmask\" value=\"");
    html_text(req, cfg->netmask);
    httpd_resp_sendstr_chunk(req, "\"></label></div><div class=\"row\"><label>Gateway <input name=\"gateway\" value=\"");
    html_text(req, cfg->gateway);
    httpd_resp_sendstr_chunk(req, "\"></label><label>DNS <input name=\"dns\" value=\"");
    html_text(req, cfg->dns);
    httpd_resp_sendstr_chunk(req, "\" placeholder=\"Defaults to the gateway\"></label></div>"
                                  "<button type=\"button\" id=\"apply-network\">Apply network</button> "
                                  "<span class=\"hint\" id=\"network-status\"></span></fieldset><fieldset><legend>Bambuddy</legend><label>Endpoint <input name=\"endpoint\" value=\"");
    html_text(req, cfg->endpoint);
    httpd_resp_sendstr_chunk(req, "\"></label><label>API key <input type=\"password\" name=\"api_key\" autocomplete=\"new-password\" "
                                  "placeholder=\"Leave blank to keep current\"></label><label class=\"check\"><input type=\"checkbox\" name=\"ignore_ssl\" value=\"1\"");
    if (cfg->ignore_ssl) {
        httpd_resp_sendstr_chunk(req, " checked");
    }
    httpd_resp_sendstr_chunk(req, "> Ignore SSL errors</label><label>Printer <select id=\"printer_id\" name=\"printer_id\">"
                                  "<option value=\"0\">Not set</option>");
    if (cfg->printer_id > 0) {
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        httpd_resp_sendstr_chunk(req, printer);
        httpd_resp_sendstr_chunk(req, "\" selected>Current printer (");
        httpd_resp_sendstr_chunk(req, printer);
        httpd_resp_sendstr_chunk(req, ")</option>");
    }
    httpd_resp_sendstr_chunk(req, "</select></label><button type=\"button\" class=\"ghost\" id=\"load-printers\">Load printers from Bambuddy</button> "
                                  "<span class=\"hint\" id=\"printer-status\"></span><hr><div class=\"row\">");
    send_gpio_select(req, "LED", "led_gpio", cfg->led_gpio, BB_LED_GPIO_DEFAULT);
    send_gpio_select(req, "Button", "button_gpio", cfg->button_gpio, BB_BUTTON_GPIO_DEFAULT);
    httpd_resp_sendstr_chunk(req, "</div><button type=\"submit\">Save and restart</button></fieldset></form>");
    httpd_resp_sendstr_chunk(req, "<fieldset><legend>Firmware</legend><p>Running version ");
    html_text(req, app->version);
    httpd_resp_sendstr_chunk(req, ". Upload <code>build/bambutton-idf.bin</code>.</p>"
                                  "<form method=\"post\" action=\"/api/ota\" enctype=\"multipart/form-data\">"
                                  "<label class=\"check\"><input type=\"checkbox\" name=\"force\" value=\"1\"> Force update</label>"
                                  "<label>Firmware file <input type=\"file\" name=\"firmware\" accept=\".bin\" required></label>"
                                  "<button type=\"submit\">Upload and restart</button></form></fieldset>"
                                  "<fieldset><legend>Factory reset</legend>"
                                  "<p>Erases every stored setting and restarts into the setup access point.</p>"
                                  "<form method=\"post\" action=\"/factory-reset\">"
                                  "<label class=\"check\"><input type=\"checkbox\" name=\"confirm\" value=\"1\"> Erase all settings</label>"
                                  "<button type=\"submit\" class=\"danger\">Factory reset and restart</button></form></fieldset>"
                                  "<p><a href=\"/debug\">Debug</a></p><script>"
                                  "const select=document.getElementById('printer_id');"
                                  "const status=document.getElementById('printer-status');"
                                  "document.getElementById('load-printers').addEventListener('click',async()=>{"
                                  "status.textContent=' Loading...';"
                                  "try{const current=String(select.value);const body=new URLSearchParams();"
                                  "body.set('endpoint',document.querySelector('[name=endpoint]').value);"
                                  "const key=document.querySelector('[name=api_key]').value;if(key)body.set('api_key',key);"
                                  "if(document.querySelector('[name=ignore_ssl]').checked)body.set('ignore_ssl','1');"
                                  "const response=await fetch('/api/printers',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
                                  "const data=await response.json();if(!response.ok)throw new Error(data.error||'Bambuddy rejected the request');"
                                  "const printers=Array.isArray(data)?data:(data.printers||data.results||data.items||[]);"
                                  "const valid=printers.filter(p=>p.id!==undefined);if(!valid.length)throw new Error('No printers returned by Bambuddy');"
                                  "select.replaceChildren();let found=false;valid.forEach(p=>{const option=document.createElement('option');"
                                  "option.value=p.id;option.selected=String(p.id)===current;found=found||option.selected;"
                                  "option.textContent=(p.friendly_name||p.name||p.display_name||'Printer')+' ('+p.id+')';select.appendChild(option);});"
                                  "if(!found&&current!=='0'){const option=document.createElement('option');option.value=current;option.selected=true;"
                                  "option.textContent='Current printer ('+current+', not returned)';select.insertBefore(option,select.firstChild);}"
                                  "status.textContent=' Loaded.';}catch(error){status.textContent=' '+error.message;}});"
                                  "const netStatus=document.getElementById('network-status');"
                                  "const staSsid=document.getElementById('sta-ssid');"
                                  "const staSignal=document.getElementById('sta-signal');"
                                  "const staIp=document.getElementById('sta-ip');"
                                  "const staMask=document.getElementById('sta-mask');"
                                  "const staIpCell=document.getElementById('sta-ip-cell');"
                                  "const staMaskCell=document.getElementById('sta-mask-cell');"
                                  "const apNote=document.getElementById('ap-note');"
                                  "document.getElementById('apply-network').addEventListener('click',async()=>{"
                                  "netStatus.textContent=' Joining...';"
                                  "try{const body=new URLSearchParams();"
                                  "body.set('wifi_ssid',document.querySelector('[name=wifi_ssid]').value);"
                                  "const password=document.querySelector('[name=wifi_password]').value;"
                                  "if(password)body.set('wifi_password',password);"
                                  "if(document.querySelector('[name=use_dhcp]').checked)body.set('use_dhcp','1');"
                                  "body.set('ip',document.querySelector('[name=ip]').value);"
                                  "body.set('netmask',document.querySelector('[name=netmask]').value);"
                                  "body.set('gateway',document.querySelector('[name=gateway]').value);"
                                  "body.set('dns',document.querySelector('[name=dns]').value);"
                                  "const response=await fetch('/apply-network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
                                  "const data=await response.json();"
                                  "if(!response.ok)throw new Error(data.error||'Network was not applied');"
                                  "netStatus.textContent=' '+data.message;"
                                  "if(data.joined){staSsid.textContent=data.ssid;staSignal.textContent=data.signal||'';staIp.textContent=data.ip;staMask.textContent=data.netmask;staIpCell.hidden=false;staMaskCell.hidden=false;}"
                                  "else{staSsid.textContent='Not joined';staSignal.textContent='';staIpCell.hidden=true;staMaskCell.hidden=true;}"
                                  "if(data.ap)apNote.textContent=data.joined?'Setup access point is on at 192.168.4.1. It turns off 60 seconds after the station stays joined.':'Setup access point is on at 192.168.4.1.';"
                                  "}catch(error){netStatus.textContent=' '+error.message;}});"
                                  "</script></body></html>");
    end_html(req);
}

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body is too large");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request body");
            return false;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t root_get(httpd_req_t *req)
{
    bb_config_t cfg;
    bb_web_copy_config(&cfg);
    send_config_page(req, &cfg, "", 200);
    return ESP_OK;
}

static bool parse_printer_id(const char *text, int32_t *out, char *err, size_t err_len)
{
    if (text == NULL || text[0] == '\0' || strcmp(text, "0") == 0) {
        *out = 0;
        return true;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 1000000) {
        snprintf(err, err_len, "Printer must be a positive number");
        return false;
    }
    *out = (int32_t)value;
    return true;
}

static bool parse_gpio(const char *text, uint8_t *out, const char *label, char *err, size_t err_len)
{
    char *end = NULL;
    long value = strtol(text != NULL ? text : "", &end, 10);
    if (text == NULL || end == text || *end != '\0' || value < 0 || value > 255 ||
        !bb_config_gpio_allowed((uint8_t)value)) {
        snprintf(err, err_len, "%s GPIO is not available on this board", label);
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[2048];
    if (!read_small_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    char ssid[BB_SSID_CAP];
    char password[BB_PASSWORD_CAP];
    char endpoint[BB_ENDPOINT_CAP];
    char api_key[BB_API_KEY_CAP];
    char printer_text[16];
    char ignore_text[8];
    char dhcp_text[8];
    char ip[BB_IPV4_CAP];
    char netmask[BB_IPV4_CAP];
    char gateway[BB_IPV4_CAP];
    char dns[BB_IPV4_CAP];
    char led_text[8];
    char button_text[8];
    char err[128];
    form_status_t ssid_status = form_value(body, "wifi_ssid", ssid, sizeof(ssid));
    form_status_t password_status = form_value(body, "wifi_password", password, sizeof(password));
    form_status_t endpoint_status = form_value(body, "endpoint", endpoint, sizeof(endpoint));
    form_status_t api_key_status = form_value(body, "api_key", api_key, sizeof(api_key));
    form_status_t printer_status = form_value(body, "printer_id", printer_text, sizeof(printer_text));
    form_status_t ignore_status = form_value(body, "ignore_ssl", ignore_text, sizeof(ignore_text));
    form_status_t dhcp_status = form_value(body, "use_dhcp", dhcp_text, sizeof(dhcp_text));
    form_status_t ip_status = form_value(body, "ip", ip, sizeof(ip));
    form_status_t mask_status = form_value(body, "netmask", netmask, sizeof(netmask));
    form_status_t gateway_status = form_value(body, "gateway", gateway, sizeof(gateway));
    form_status_t dns_status = form_value(body, "dns", dns, sizeof(dns));
    form_status_t led_status = form_value(body, "led_gpio", led_text, sizeof(led_text));
    form_status_t button_status = form_value(body, "button_gpio", button_text, sizeof(button_text));
    if (!field_fits(ssid_status, err, sizeof(err), "Wi-Fi SSID is too long") ||
        !field_fits(password_status, err, sizeof(err), "Wi-Fi password is too long") ||
        !field_fits(endpoint_status, err, sizeof(err), "Endpoint is too long") ||
        !field_fits(api_key_status, err, sizeof(err), "API key is too long") ||
        !field_fits(printer_status, err, sizeof(err), "Printer must be a positive number") ||
        !field_fits(ignore_status, err, sizeof(err), "A form field is too long") ||
        !field_fits(dhcp_status, err, sizeof(err), "A form field is too long") ||
        !field_fits(ip_status, err, sizeof(err), "IP address is too long") ||
        !field_fits(mask_status, err, sizeof(err), "Netmask is too long") ||
        !field_fits(gateway_status, err, sizeof(err), "Gateway is too long") ||
        !field_fits(dns_status, err, sizeof(err), "DNS is too long") ||
        !field_fits(led_status, err, sizeof(err), "LED GPIO is not available on this board") ||
        !field_fits(button_status, err, sizeof(err), "Button GPIO is not available on this board")) {
        bb_config_t cfg;
        bb_web_copy_config(&cfg);
        send_config_page(req, &cfg, err, 400);
        return ESP_OK;
    }
    bool password_set = password_status == FORM_OK && password[0] != '\0';
    bool api_key_set = api_key_status == FORM_OK && api_key[0] != '\0';
    bool ignore_ssl = ignore_status == FORM_OK;
    bool use_dhcp = dhcp_status == FORM_OK;

    int32_t printer_id = 0;
    uint8_t led_gpio = 0;
    uint8_t button_gpio = 0;
    if (!parse_printer_id(printer_text, &printer_id, err, sizeof(err)) ||
        !parse_gpio(led_text, &led_gpio, "LED", err, sizeof(err)) ||
        !parse_gpio(button_text, &button_gpio, "Button", err, sizeof(err))) {
        bb_config_t cfg;
        bb_web_copy_config(&cfg);
        send_config_page(req, &cfg, err, 400);
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bb_config_t updated = *s_cfg;
    bool ok = bb_config_update(&updated, ssid, password, password_set, endpoint, api_key, api_key_set,
                               ignore_ssl, use_dhcp, ip, netmask, gateway, dns, printer_id, led_gpio,
                               button_gpio, err, sizeof(err));
    if (ok) {
        esp_err_t saved = bb_nvs_save(&updated);
        if (saved != ESP_OK) {
            snprintf(err, sizeof(err), "Could not store settings (%s)", esp_err_to_name(saved));
            ok = false;
        } else {
            *s_cfg = updated;
        }
    }
    bb_config_t page_cfg = *s_cfg;
    xSemaphoreGive(s_lock);

    if (!ok) {
        send_config_page(req, &page_cfg, err, 400);
        return ESP_OK;
    }
    send_config_page(req, &page_cfg, "Settings saved. The board will restart.", 200);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static void send_json_error(httpd_req_t *req, int status, const char *message);

static esp_err_t apply_network_post(httpd_req_t *req)
{
    char body[1024];
    if (!read_small_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    char ssid[BB_SSID_CAP];
    char password[BB_PASSWORD_CAP];
    char dhcp_text[8];
    char ip[BB_IPV4_CAP];
    char netmask[BB_IPV4_CAP];
    char gateway[BB_IPV4_CAP];
    char dns[BB_IPV4_CAP];
    char err[128];
    form_status_t ssid_status = form_value(body, "wifi_ssid", ssid, sizeof(ssid));
    form_status_t password_status = form_value(body, "wifi_password", password, sizeof(password));
    form_status_t dhcp_status = form_value(body, "use_dhcp", dhcp_text, sizeof(dhcp_text));
    form_status_t ip_status = form_value(body, "ip", ip, sizeof(ip));
    form_status_t mask_status = form_value(body, "netmask", netmask, sizeof(netmask));
    form_status_t gateway_status = form_value(body, "gateway", gateway, sizeof(gateway));
    form_status_t dns_status = form_value(body, "dns", dns, sizeof(dns));
    if (!field_fits(ssid_status, err, sizeof(err), "Wi-Fi SSID is too long") ||
        !field_fits(password_status, err, sizeof(err), "Wi-Fi password is too long") ||
        !field_fits(dhcp_status, err, sizeof(err), "A form field is too long") ||
        !field_fits(ip_status, err, sizeof(err), "IP address is too long") ||
        !field_fits(mask_status, err, sizeof(err), "Netmask is too long") ||
        !field_fits(gateway_status, err, sizeof(err), "Gateway is too long") ||
        !field_fits(dns_status, err, sizeof(err), "DNS is too long")) {
        send_json_error(req, 400, err);
        return ESP_OK;
    }
    if (ssid[0] == '\0') {
        send_json_error(req, 400, "Wi-Fi SSID is required");
        return ESP_OK;
    }

    bool password_set = password_status == FORM_OK && password[0] != '\0';
    bool use_dhcp = dhcp_status == FORM_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bb_config_t updated = *s_cfg;
    bool ok = bb_config_update(&updated, ssid, password, password_set, updated.endpoint, updated.api_key, false,
                               updated.ignore_ssl, use_dhcp, ip, netmask, gateway, dns, updated.printer_id,
                               updated.led_gpio, updated.button_gpio, err, sizeof(err));
    if (ok) {
        esp_err_t saved = bb_nvs_save(&updated);
        if (saved != ESP_OK) {
            snprintf(err, sizeof(err), "Could not store settings (%s)", esp_err_to_name(saved));
            ok = false;
        } else {
            *s_cfg = updated;
        }
    }
    xSemaphoreGive(s_lock);
    if (!ok) {
        send_json_error(req, 400, err);
        return ESP_OK;
    }

    bool joined = false;
    esp_err_t applied = bb_wifi_apply_station(&updated, &joined);
    char station_ssid[33] = "";
    char station_ip[16] = "";
    char station_mask[16] = "";
    char signal[32] = "";
    if (joined) {
        bb_wifi_get_station(station_ssid, sizeof(station_ssid), station_ip, sizeof(station_ip), station_mask,
                            sizeof(station_mask));
        bb_wifi_format_signal(signal, sizeof(signal));
    }
    const char *message = "Joined the station.";
    if (applied != ESP_OK) {
        message = "Saved, but the station could not be started.";
    } else if (!joined) {
        message = "Saved. The station has not joined yet. The board will keep trying.";
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        send_json_error(req, 502, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "joined", joined);
    cJSON_AddBoolToObject(root, "ap", bb_wifi_ap_active());
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "ssid", station_ssid);
    cJSON_AddStringToObject(root, "ip", station_ip);
    cJSON_AddStringToObject(root, "netmask", station_mask);
    cJSON_AddStringToObject(root, "signal", signal);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, printed != NULL ? printed : "{\"joined\":false,\"message\":\"Request failed\"}");
    free(printed);
    return ESP_OK;
}

static void send_json_error(httpd_req_t *req, int status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message != NULL ? message : "Request failed");
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" : "502 Bad Gateway");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, printed != NULL ? printed : "{\"error\":\"Request failed\"}");
    free(printed);
}

static esp_err_t printers_post(httpd_req_t *req)
{
    char form[1024];
    if (!read_small_body(req, form, sizeof(form))) {
        return ESP_OK;
    }
    if (!bb_wifi_sta_connected()) {
        send_json_error(req, 502, "Setup AP has no route to Bambuddy");
        return ESP_OK;
    }

    bb_config_t cfg;
    bb_web_copy_config(&cfg);
    char endpoint[BB_ENDPOINT_CAP];
    char api_key[BB_API_KEY_CAP];
    char ignore_text[8];
    form_status_t endpoint_status = form_value(form, "endpoint", endpoint, sizeof(endpoint));
    form_status_t api_key_status = form_value(form, "api_key", api_key, sizeof(api_key));
    form_status_t ignore_status = form_value(form, "ignore_ssl", ignore_text, sizeof(ignore_text));
    if (endpoint_status == FORM_TOO_LONG) {
        send_json_error(req, 400, "Endpoint is too long");
        return ESP_OK;
    }
    if (api_key_status == FORM_TOO_LONG) {
        send_json_error(req, 400, "API key is too long");
        return ESP_OK;
    }
    if (ignore_status == FORM_TOO_LONG) {
        send_json_error(req, 400, "A form field is too long");
        return ESP_OK;
    }
    if (endpoint_status == FORM_OK && endpoint[0] != '\0') {
        memcpy(cfg.endpoint, endpoint, strlen(endpoint) + 1);
    }
    if (api_key_status == FORM_OK && api_key[0] != '\0') {
        memcpy(cfg.api_key, api_key, strlen(api_key) + 1);
    }
    cfg.ignore_ssl = ignore_status == FORM_OK ? 1 : 0;
    if (cfg.endpoint[0] == '\0' || cfg.api_key[0] == '\0') {
        send_json_error(req, 400, "Endpoint and API key are required");
        return ESP_OK;
    }

    char *body = malloc(8192);
    char err[128];
    if (body == NULL) {
        send_json_error(req, 502, "Out of memory");
        return ESP_OK;
    }
    esp_err_t result = bb_api_get_printers(&cfg, body, 8192, err, sizeof(err));
    if (result != ESP_OK) {
        free(body);
        send_json_error(req, 502, err[0] != '\0' ? err : "Bambuddy request failed");
        return ESP_OK;
    }
    if (body[0] != '{' && body[0] != '[') {
        free(body);
        send_json_error(req, 502, "Bambuddy did not return JSON");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    char body[256];
    if (!read_small_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    char confirm[8];
    form_status_t confirm_status = form_value(body, "confirm", confirm, sizeof(confirm));
    if (confirm_status != FORM_OK || strcmp(confirm, "1") != 0) {
        bb_config_t cfg;
        bb_web_copy_config(&cfg);
        send_config_page(req, &cfg, "Check the box to erase all settings.", 400);
        return ESP_OK;
    }

    esp_err_t err = bb_nvs_erase();
    if (err != ESP_OK) {
        bb_config_t cfg;
        bb_web_copy_config(&cfg);
        char message[96];
        snprintf(message, sizeof(message), "Could not erase settings (%s)", esp_err_to_name(err));
        send_config_page(req, &cfg, message, 400);
        return ESP_OK;
    }

    bb_config_t blank;
    bb_config_set_defaults(&blank);
    send_config_page(req, &blank, "Settings erased. The board will restart.", 200);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t debug_get(httpd_req_t *req)
{
    bb_runtime_status_t runtime = {0};
    if (s_runtime != NULL) {
        s_runtime(&runtime);
    }
    bb_config_t cfg;
    bb_web_copy_config(&cfg);
    char ip[16];
    bb_wifi_get_ip(ip, sizeof(ip));
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    begin_html(req);
    httpd_resp_sendstr_chunk(req, "<h1>BamButton-IDF debug</h1><ul>");
    httpd_resp_sendstr_chunk(req, "<li>Mode: ");
    if (bb_wifi_sta_connected() && bb_wifi_ap_active()) {
        httpd_resp_sendstr_chunk(req, "station and access point");
    } else if (bb_wifi_ap_active()) {
        httpd_resp_sendstr_chunk(req, "access point");
    } else {
        httpd_resp_sendstr_chunk(req, "station");
    }
    httpd_resp_sendstr_chunk(req, "</li><li>Wi-Fi connected: ");
    httpd_resp_sendstr_chunk(req, bb_wifi_sta_connected() ? "yes" : "no");
    httpd_resp_sendstr_chunk(req, "</li><li>IP address: ");
    html_text(req, ip[0] != '\0' ? ip : "none");
    char signal[32];
    httpd_resp_sendstr_chunk(req, "</li><li>Signal: ");
    html_text(req, bb_wifi_format_signal(signal, sizeof(signal)) ? signal : "none");
    httpd_resp_sendstr_chunk(req, cfg.use_dhcp ? "</li><li>Station address: DHCP" : "</li><li>Station address: static");
    httpd_resp_sendstr_chunk(req, "</li><li>Hostname: ");
    html_text(req, bb_wifi_hostname());
    char line[96];
    snprintf(line, sizeof(line), "</li><li>LED GPIO: %u</li><li>Button GPIO: %u</li>", cfg.led_gpio,
             cfg.button_gpio);
    httpd_resp_sendstr_chunk(req, line);
    httpd_resp_sendstr_chunk(req, "<li>Endpoint: ");
    html_text(req, cfg.endpoint[0] != '\0' ? cfg.endpoint : "not set");
    httpd_resp_sendstr_chunk(req, "</li><li>Ignore SSL errors: ");
    httpd_resp_sendstr_chunk(req, cfg.ignore_ssl ? "yes" : "no");
    snprintf(line, sizeof(line), "</li><li>Printer: %d</li><li>Awaiting plate clear: %s</li>",
             (int)cfg.printer_id, runtime.awaiting_plate_clear ? "yes" : "no");
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "<li>Button press pending: %s</li><li>Chamber light on: %s</li>",
             runtime.pending_press ? "yes" : "no", runtime.chamber_light ? "yes" : "no");
    httpd_resp_sendstr_chunk(req, line);
    httpd_resp_sendstr_chunk(req, "<li>Application version: ");
    html_text(req, app->version);
    httpd_resp_sendstr_chunk(req, "</li><li>Boot partition: ");
    html_text(req, running != NULL ? running->label : "unknown");
    httpd_resp_sendstr_chunk(req, "</li></ul><p><a href=\"/\">Configuration</a></p></body></html>");
    end_html(req);
    return ESP_OK;
}

#define MP_IO 1024
#define MP_CARRY (MP_IO + 128)

typedef struct {
    httpd_req_t *req;
    size_t remaining;
    char boundary[70];
    size_t boundary_len;
    char delim[80];
    size_t delim_len;
    char carry[MP_CARRY];
    size_t carry_len;
    bool in_part_body;
} multipart_t;

typedef struct {
    multipart_t *mp;
} part_reader_t;

static int mp_recv(multipart_t *mp, char *buf, size_t len)
{
    size_t out = 0;
    while (out < len) {
        if (mp->carry_len > 0) {
            size_t n = mp->carry_len;
            if (n > len - out) {
                n = len - out;
            }
            memcpy(buf + out, mp->carry, n);
            memmove(mp->carry, mp->carry + n, mp->carry_len - n);
            mp->carry_len -= n;
            out += n;
            continue;
        }
        if (mp->remaining == 0) {
            break;
        }
        size_t to_read = len - out;
        if (to_read > mp->remaining) {
            to_read = mp->remaining;
        }
        int ret = httpd_req_recv(mp->req, buf + out, to_read);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret < 0) {
            return out > 0 ? (int)out : ret;
        }
        if (ret == 0) {
            break;
        }
        mp->remaining -= (size_t)ret;
        out += (size_t)ret;
    }
    return (int)out;
}

static bool mp_boundary(httpd_req_t *req, multipart_t *mp)
{
    char content_type[256];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return false;
    }
    char *pos = strstr(content_type, "boundary=");
    if (pos == NULL) {
        return false;
    }
    pos += strlen("boundary=");
    while (*pos == ' ' || *pos == '"') {
        pos++;
    }
    size_t i = 0;
    while (*pos != '\0' && *pos != ';' && *pos != '"' && *pos != ' ' && i + 1 < sizeof(mp->boundary)) {
        mp->boundary[i++] = *pos++;
    }
    if (*pos != '\0' && *pos != ';' && *pos != '"' && *pos != ' ') {
        return false;
    }
    mp->boundary[i] = '\0';
    mp->boundary_len = i;
    if (i == 0 || i + 4 >= sizeof(mp->delim)) {
        return false;
    }
    memcpy(mp->delim, "\r\n--", 4);
    memcpy(mp->delim + 4, mp->boundary, i + 1);
    mp->delim_len = i + 4;
    return true;
}

static esp_err_t mp_read_line(multipart_t *mp, char *line, size_t cap)
{
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c = 0;
        int got = mp_recv(mp, &c, 1);
        if (got < 0) {
            return ESP_FAIL;
        }
        if (got == 0) {
            line[pos] = '\0';
            return pos > 0 ? ESP_OK : ESP_FAIL;
        }
        line[pos++] = c;
        if (pos >= 2 && line[pos - 2] == '\r' && line[pos - 1] == '\n') {
            line[pos] = '\0';
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static void mp_part_name(const char *line, char *name, size_t name_cap)
{
    const char *token = "name=\"";
    const char *pos = strstr(line, token);
    if (pos == NULL || name_cap == 0) {
        return;
    }
    pos += strlen(token);
    size_t i = 0;
    while (*pos != '\0' && *pos != '"' && i + 1 < name_cap) {
        name[i++] = *pos++;
    }
    if (*pos != '"') {
        name[0] = '\0';
        return;
    }
    name[i] = '\0';
}

static int part_stream_read(void *ctx, char *buf, size_t max_len)
{
    part_reader_t *pr = ctx;
    multipart_t *mp = pr->mp;
    size_t written = 0;
    const size_t keep = mp->delim_len > 0 ? mp->delim_len - 1 : 0;

    while (written < max_len && mp->in_part_body) {
        char *hit = NULL;
        if (mp->carry_len >= mp->delim_len) {
            for (size_t i = 0; i + mp->delim_len <= mp->carry_len; i++) {
                if (memcmp(mp->carry + i, mp->delim, mp->delim_len) == 0) {
                    hit = mp->carry + i;
                    break;
                }
            }
        }
        if (hit != NULL) {
            size_t safe = (size_t)(hit - mp->carry);
            size_t n = safe;
            if (n > max_len - written) {
                n = max_len - written;
            }
            if (n > 0) {
                memcpy(buf + written, mp->carry, n);
                written += n;
                memmove(mp->carry, mp->carry + n, mp->carry_len - n);
                mp->carry_len -= n;
            }
            if (n < safe) {
                return (int)written;
            }
            mp->in_part_body = false;
            return (int)written;
        }
        if (mp->carry_len > keep) {
            size_t n = mp->carry_len - keep;
            if (n > max_len - written) {
                n = max_len - written;
            }
            memcpy(buf + written, mp->carry, n);
            written += n;
            memmove(mp->carry, mp->carry + n, mp->carry_len - n);
            mp->carry_len -= n;
            continue;
        }
        char chunk[MP_IO];
        int got = mp_recv(mp, chunk, sizeof(chunk));
        if (got < 0) {
            return got;
        }
        if (got == 0) {
            if (mp->carry_len > 0 && written < max_len) {
                size_t n = mp->carry_len;
                if (n > max_len - written) {
                    n = max_len - written;
                }
                memcpy(buf + written, mp->carry, n);
                written += n;
                memmove(mp->carry, mp->carry + n, mp->carry_len - n);
                mp->carry_len -= n;
                if (mp->carry_len > 0) {
                    return (int)written;
                }
            }
            mp->in_part_body = false;
            break;
        }
        if (mp->carry_len + (size_t)got > sizeof(mp->carry)) {
            return -1;
        }
        memcpy(mp->carry + mp->carry_len, chunk, (size_t)got);
        mp->carry_len += (size_t)got;
    }
    return (int)written;
}

static esp_err_t mp_finish_boundary(multipart_t *mp, bool *more)
{
    char probe[80];
    int got = mp_recv(mp, probe, mp->delim_len);
    if (got != (int)mp->delim_len || memcmp(probe, mp->delim, mp->delim_len) != 0) {
        return ESP_FAIL;
    }
    char tail[2] = {0};
    got = mp_recv(mp, tail, 2);
    if (got == 2 && tail[0] == '-' && tail[1] == '-') {
        *more = false;
        return ESP_OK;
    }
    if (got == 2 && tail[0] == '\r' && tail[1] == '\n') {
        *more = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}

static void mp_drain(multipart_t *mp)
{
    part_reader_t pr = {.mp = mp};
    mp->in_part_body = true;
    char scratch[256];
    while (mp->in_part_body) {
        if (part_stream_read(&pr, scratch, sizeof(scratch)) <= 0) {
            break;
        }
    }
}

static bool token_is(const char *body, const char *token)
{
    size_t n = strlen(token);
    if (strncmp(body, token, n) != 0) {
        return false;
    }
    char c = body[n];
    return c == '\0' || c == '\r' || c == '\n' || c == ' ';
}

static bool mp_read_force(multipart_t *mp)
{
    part_reader_t pr = {.mp = mp};
    mp->in_part_body = true;
    char body[32] = {0};
    size_t total = 0;
    while (mp->in_part_body && total + 1 < sizeof(body)) {
        int got = part_stream_read(&pr, body + total, sizeof(body) - 1 - total);
        if (got <= 0) {
            break;
        }
        total += (size_t)got;
    }
    while (mp->in_part_body) {
        char scratch[64];
        if (part_stream_read(&pr, scratch, sizeof(scratch)) <= 0) {
            break;
        }
    }
    return token_is(body, "1") || token_is(body, "true") || token_is(body, "on") || token_is(body, "yes");
}

static esp_err_t mp_headers(multipart_t *mp, char *name, size_t name_cap)
{
    char line[512];
    name[0] = '\0';
    for (;;) {
        if (mp_read_line(mp, line, sizeof(line)) != ESP_OK) {
            return ESP_FAIL;
        }
        if (line[0] == '\r' && line[1] == '\n') {
            return ESP_OK;
        }
        if (line[0] == '\0') {
            return ESP_FAIL;
        }
        mp_part_name(line, name, name_cap);
    }
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing firmware body");
        return ESP_FAIL;
    }
    if (!bb_ota_begin_session()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "A firmware update is already running");
        return ESP_FAIL;
    }

    multipart_t mp = {0};
    mp.req = req;
    mp.remaining = req->content_len;
    if (!mp_boundary(req, &mp)) {
        bb_ota_end_session();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type must be multipart/form-data");
        return ESP_FAIL;
    }

    char line[128];
    char expect[80];
    if (mp.boundary_len + 2 >= sizeof(expect)) {
        bb_ota_end_session();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid multipart body");
        return ESP_FAIL;
    }
    memcpy(expect, "--", 2);
    memcpy(expect + 2, mp.boundary, mp.boundary_len + 1);
    if (mp_read_line(&mp, line, sizeof(line)) != ESP_OK || strncmp(line, expect, mp.boundary_len + 2) != 0) {
        bb_ota_end_session();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid multipart body");
        return ESP_FAIL;
    }

    bool force = false;
    bool wrote = false;
    bool more = true;
    esp_err_t update = ESP_ERR_NOT_FOUND;
    char err[BB_OTA_ERR_CAP] = "Firmware file is missing";

    while (more) {
        char name[32] = {0};
        if (mp_headers(&mp, name, sizeof(name)) != ESP_OK) {
            bb_ota_end_session();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid multipart headers");
            return ESP_FAIL;
        }
        if (strcmp(name, "force") == 0) {
            force = mp_read_force(&mp);
        } else if (strcmp(name, "firmware") == 0) {
            part_reader_t pr = {.mp = &mp};
            mp.in_part_body = true;
            bb_ota_reader_t reader = {.read = part_stream_read, .ctx = &pr};
            update = bb_ota_update_from_reader(&reader, force, err, sizeof(err));
            wrote = true;
            if (update != ESP_OK) {
                bb_ota_end_session();
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
                return ESP_FAIL;
            }
        } else {
            mp_drain(&mp);
        }
        if (mp_finish_boundary(&mp, &more) != ESP_OK) {
            if (wrote && update == ESP_OK) {
                more = false;
                break;
            }
            bb_ota_end_session();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed multipart body");
            return ESP_FAIL;
        }
    }

    if (!wrote || update != ESP_OK) {
        bb_ota_end_session();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, err);
    ESP_LOGI(TAG, "%s", err);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

void bb_web_copy_config(bb_config_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL || s_cfg == NULL) {
        bb_config_set_defaults(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t bb_web_start(bb_config_t *cfg, bb_runtime_fn runtime)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;
    s_runtime = runtime;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 12288;
    http.recv_wait_timeout = 30;
    http.send_wait_timeout = 30;
    http.max_uri_handlers = 8;
    http.lru_purge_enable = true;
    http.max_open_sockets = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/apply-network", .method = HTTP_POST, .handler = apply_network_post},
        {.uri = "/api/printers", .method = HTTP_POST, .handler = printers_post},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/factory-reset", .method = HTTP_POST, .handler = factory_reset_post},
        {.uri = "/debug", .method = HTTP_GET, .handler = debug_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "Web server on port 80");
    return ESP_OK;
}
