// GET  /api/status        device state
// GET  /api/log           last commands with timings
// POST /api/input/1|2     switch input (api mode only)
// POST /api/key/KEY_NAME  raw remote key (api mode only)
// POST /api/pair          re-pair with the TV (any mode)
// POST /api/power         toggle TV power (any mode): KEY_POWER or Wake-on-LAN
// GET  /                  status page

#include "api.h"

#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "status.h"
#include "tv.h"

static const char *TAG = "api";

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Monitor Control</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem}"
    "button{font-size:1.2rem;padding:.8rem 1.4rem;margin:.3rem .3rem .3rem 0}"
    "pre{background:#f3f3f3;padding:.8rem;overflow:auto}"
    "@media(prefers-color-scheme:dark){body{background:#111;color:#eee}pre{background:#222}}"
    "</style></head><body><h1>Monitor Control</h1>"
    "<p>Mode: <b id=mode>?</b></p>"
    "<button id=b1 onclick=go(1)>HDMI 1</button><button id=b2 onclick=go(2)>HDMI 2</button>"
    "<p><button onclick=power()>Power</button> <button onclick=pair()>Pair with TV</button></p><pre id=st></pre><script>"
    "async function refresh(){try{const s=await(await fetch('/api/status')).json();"
    "mode.textContent=s.mode+(s.busy?' (busy)':'');"
    "b1.disabled=b2.disabled=s.mode!='api';st.textContent=JSON.stringify(s,null,2)}catch(e){}}"
    "async function go(n){await fetch('/api/input/'+n,{method:'POST'});refresh()}"
    "async function power(){await fetch('/api/power',{method:'POST'});refresh()}"
    "async function pair(){if(confirm('Re-pair? Accept the prompt on the TV.'))"
    "await fetch('/api/pair',{method:'POST'});refresh()}"
    "refresh();setInterval(refresh,2000)</script></body></html>";

// The API is for the Wi-Fi side; the TV link is not allowed to drive it.
static bool from_tv_link(httpd_req_t *req)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    esp_ip4_addr_t tv_link;
    esp_netif_str_to_ip4(TV_NET_DEVICE_IP, &tv_link);
    if (addr.sin6_family == AF_INET) {
        return ((struct sockaddr_in *)&addr)->sin_addr.s_addr == tv_link.addr;
    }
    // IPv4-mapped IPv6 (::ffff:a.b.c.d)
    return addr.sin6_family == AF_INET6 && addr.sin6_addr.un.u32_addr[3] == tv_link.addr;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *body)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", error);
    return send_json(req, status, body);
}

// Refuses API control unless the toggle is centered.
static bool locked(httpd_req_t *req)
{
    status_t st;
    status_get(&st);
    if (st.mode == MODE_API) {
        return false;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", "locked");
    cJSON_AddStringToObject(body, "mode", status_mode_name(st.mode));
    send_json(req, "409 Conflict", body);
    return true;
}

static esp_err_t send_accepted(httpd_req_t *req, const char *what)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "queued", what);
    return send_json(req, "202 Accepted", body);
}

#define GUARD(req)                                                   \
    do {                                                             \
        if (from_tv_link(req)) {                                     \
            return send_error(req, "403 Forbidden", "tv link");      \
        }                                                            \
    } while (0)

static esp_err_t page_get(httpd_req_t *req)
{
    GUARD(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    GUARD(req);
    status_t st;
    status_get(&st);

    cJSON *body = cJSON_CreateObject();
    if (st.input) {
        cJSON_AddNumberToObject(body, "input", st.input);
    } else {
        cJSON_AddNullToObject(body, "input");
    }
    cJSON_AddStringToObject(body, "mode", status_mode_name(st.mode));
    cJSON_AddBoolToObject(body, "busy", st.busy);

    cJSON *tv = cJSON_AddObjectToObject(body, "tv");
    cJSON_AddStringToObject(tv, "ip", TV_IP);
    cJSON_AddBoolToObject(tv, "link", st.eth_link);
    cJSON_AddBoolToObject(tv, "leased", st.tv_leased);
    if (st.tv_mac_known) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", st.tv_mac[0], st.tv_mac[1],
                 st.tv_mac[2], st.tv_mac[3], st.tv_mac[4], st.tv_mac[5]);
        cJSON_AddStringToObject(tv, "mac", mac);
    }
    cJSON_AddBoolToObject(tv, "connected", st.tv_connected);
    cJSON_AddBoolToObject(tv, "paired", st.tv_paired);
    cJSON_AddStringToObject(tv, "power", st.tv_power);

    cJSON *dhcp = cJSON_AddObjectToObject(body, "dhcp");
    cJSON_AddBoolToObject(dhcp, "serving", st.dhcp_serving);
    cJSON_AddBoolToObject(dhcp, "other_server_seen", st.dhcp_rogue);
    cJSON_AddBoolToObject(dhcp, "subnet_conflict", st.subnet_conflict);

    cJSON *wifi = cJSON_AddObjectToObject(body, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", st.wifi_connected);
    cJSON_AddStringToObject(wifi, "ip", st.wifi_ip);

    return send_json(req, "200 OK", body);
}

static esp_err_t log_get(httpd_req_t *req)
{
    GUARD(req);
    cmd_log_entry_t entries[CMD_LOG_LEN];
    int n = cmd_log_get(entries);

    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "at_ms", (double)entries[i].at_ms);
        cJSON_AddStringToObject(e, "source", status_source_name(entries[i].source));
        cJSON_AddStringToObject(e, "command", entries[i].what);
        cJSON_AddStringToObject(e, "result", entries[i].result);
        cJSON_AddNumberToObject(e, "duration_ms", entries[i].duration_ms);
        cJSON_AddItemToArray(list, e);
    }
    return send_json(req, "200 OK", list);
}

// The path segment after `prefix`, e.g. "2" for /api/input/2.
static const char *path_tail(httpd_req_t *req, const char *prefix)
{
    return req->uri + strlen(prefix);
}

static esp_err_t input_post(httpd_req_t *req)
{
    GUARD(req);
    const char *tail = path_tail(req, "/api/input/");
    if (strcmp(tail, "1") != 0 && strcmp(tail, "2") != 0) {
        return send_error(req, "404 Not Found", "input must be 1 or 2");
    }
    if (locked(req)) {
        return ESP_OK;
    }
    int input = tail[0] - '0';
    ESP_LOGI(TAG, "input %d requested", input);
    tv_request_input(input, SRC_API);
    return send_accepted(req, tail[0] == '1' ? "input 1" : "input 2");
}

static esp_err_t key_post(httpd_req_t *req)
{
    GUARD(req);
    const char *key = path_tail(req, "/api/key/");
    size_t len = strlen(key);
    bool valid = len > 4 && len < 32 && strncmp(key, "KEY_", 4) == 0;
    for (size_t i = 0; valid && i < len; i++) {
        valid = isupper((unsigned char)key[i]) || isdigit((unsigned char)key[i]) || key[i] == '_';
    }
    if (!valid) {
        return send_error(req, "400 Bad Request", "key must look like KEY_NAME");
    }
    if (locked(req)) {
        return ESP_OK;
    }
    tv_request_key(key, SRC_API);
    return send_accepted(req, key);
}

static esp_err_t power_post(httpd_req_t *req)
{
    GUARD(req);
    tv_request_power(SRC_API);
    return send_accepted(req, "power");
}

static esp_err_t pair_post(httpd_req_t *req)
{
    GUARD(req);
    tv_request_pair(SRC_API);
    return send_accepted(req, "pair");
}

void api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/api/input/*", .method = HTTP_POST, .handler = input_post},
        {.uri = "/api/key/*", .method = HTTP_POST, .handler = key_post},
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair_post},
        {.uri = "/api/power", .method = HTTP_POST, .handler = power_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    ESP_LOGI(TAG, "HTTP API on port 80");
}
