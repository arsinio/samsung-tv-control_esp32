// Port of poc/tvctl.py.
//
// wss://TV_IP:8002/api/v2/channels/samsung.remote.control?name=<b64>[&token=<t>]
// The first connect without a token shows an Allow prompt on the TV; the
// ms.channel.connect event then carries data.token, which is kept in NVS.

#include "tv.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "eth_dhcps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include "nvs.h"

static const char *TAG = "tv";

#define NVS_NAMESPACE "tv"
#define NVS_TOKEN_KEY "token"

#define BIT_CHANNEL_CONNECT BIT0  // ms.channel.connect received
#define BIT_UNAUTHORIZED    BIT1  // ms.channel.unauthorized received
#define BIT_CLOSED          BIT2  // transport closed or failed

typedef enum { CMD_INPUT, CMD_KEY, CMD_PAIR, CMD_POWER } cmd_type_t;

typedef struct {
    cmd_type_t type;
    cmd_source_t source;
    int input;
    char key[32];
} tv_cmd_t;

static QueueHandle_t s_queue;
static EventGroupHandle_t s_events;
static esp_websocket_client_handle_t s_client;
static char s_token[64];

// ms.channel.connect lists every connected client, so it grows with them
static char s_rx[6144];
static size_t s_rx_len;
static bool s_rx_overflow;

// --- Token ----------------------------------------------------------------

static void token_load(void)
{
    nvs_handle_t nvs;
    s_token[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_token);
        if (nvs_get_str(nvs, NVS_TOKEN_KEY, s_token, &len) != ESP_OK) {
            s_token[0] = '\0';
        }
        nvs_close(nvs);
    }
    status_lock()->tv_paired = s_token[0] != '\0';
    status_unlock();
}

static void token_save(const char *token)
{
    strlcpy(s_token, token ? token : "", sizeof(s_token));
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (s_token[0]) {
            nvs_set_str(nvs, NVS_TOKEN_KEY, s_token);
        } else {
            nvs_erase_key(nvs, NVS_TOKEN_KEY);
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    status_lock()->tv_paired = s_token[0] != '\0';
    status_unlock();
}

// --- WebSocket ------------------------------------------------------------

static void set_connected(bool connected)
{
    status_lock()->tv_connected = connected;
    status_unlock();
}

static void handle_message(const char *msg, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(msg, len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable message: %.*s", (int)len, msg);
        return;
    }
    const cJSON *event = cJSON_GetObjectItem(root, "event");
    const char *name = cJSON_IsString(event) ? event->valuestring : "";

    if (strcmp(name, "ms.channel.connect") == 0) {
        const cJSON *token = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "token");
        if (cJSON_IsString(token) && strcmp(token->valuestring, s_token) != 0) {
            ESP_LOGI(TAG, "paired; token saved");
            token_save(token->valuestring);
        }
        xEventGroupSetBits(s_events, BIT_CHANNEL_CONNECT);
    } else if (strcmp(name, "ms.channel.unauthorized") == 0) {
        ESP_LOGW(TAG, "TV refused the connection (unauthorized)");
        xEventGroupSetBits(s_events, BIT_UNAUTHORIZED);
    } else if (strcmp(name, "ms.error") == 0) {
        ESP_LOGW(TAG, "TV error: %.*s", (int)len, msg);
    } else {
        ESP_LOGD(TAG, "event %s", name);
    }
    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket open; waiting for the TV");
        break;
    case WEBSOCKET_EVENT_DATA: {
        // Text frames start a message; continuation frames (op 0) append to it.
        // A frame may itself arrive in chunks (payload_offset/payload_len).
        if (d->op_code == 0x1 && d->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_overflow = false;
        } else if (d->op_code != 0x0 && d->op_code != 0x1) {
            break;
        }
        if (s_rx_len + d->data_len <= sizeof(s_rx)) {
            memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
            s_rx_len += d->data_len;
        } else {
            s_rx_overflow = true;
        }
        bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
        if (frame_done && d->fin) {
            if (s_rx_overflow) {
                ESP_LOGW(TAG, "dropped message larger than %u bytes", (unsigned)sizeof(s_rx));
            } else {
                handle_message(s_rx, s_rx_len);
            }
            s_rx_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WebSocket closed (event %ld)", (long)id);
        set_connected(false);
        xEventGroupSetBits(s_events, BIT_CLOSED);
        break;
    default:
        break;
    }
}

static void ws_close(void)
{
    if (s_client) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    set_connected(false);
}

static bool ws_is_open(void)
{
    return s_client && esp_websocket_client_is_connected(s_client) &&
           (xEventGroupGetBits(s_events) & BIT_CHANNEL_CONNECT);
}

// Percent-encodes base64 characters that aren't safe in a query string.
static void url_encode_b64(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o + 4 < out_len; in++) {
        if (*in == '+' || *in == '/' || *in == '=') {
            o += snprintf(out + o, out_len - o, "%%%02X", *in);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

// Returns "ok" or a short failure reason.
static const char *ws_open(uint32_t timeout_ms)
{
    ws_close();

    unsigned char b64[64];
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len,
                          (const unsigned char *)TV_CLIENT_NAME, strlen(TV_CLIENT_NAME));
    b64[b64_len] = '\0';
    char name[96];
    url_encode_b64((const char *)b64, name, sizeof(name));

    char uri[256];
    int n = snprintf(uri, sizeof(uri), "wss://%s:%d/api/v2/channels/samsung.remote.control?name=%s",
                     TV_IP, TV_WS_PORT, name);
    if (s_token[0]) {
        snprintf(uri + n, sizeof(uri) - n, "&token=%s", s_token);
    }

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .skip_cert_common_name_check = true,
        .network_timeout_ms = TV_NETWORK_TIMEOUT_MS,
        .disable_auto_reconnect = true,
        .buffer_size = 1024,
        .task_stack = 6144,
    };
    xEventGroupClearBits(s_events, BIT_CHANNEL_CONNECT | BIT_UNAUTHORIZED | BIT_CLOSED);
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        return "init failed";
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        ws_close();
        return "start failed";
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CHANNEL_CONNECT | BIT_UNAUTHORIZED | BIT_CLOSED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CHANNEL_CONNECT) {
        set_connected(true);
        ESP_LOGI(TAG, "connected to TV");
        return "ok";
    }
    ws_close();
    if (bits & BIT_UNAUTHORIZED) {
        token_save(NULL);  // the TV no longer accepts this token
        return "unauthorized";
    }
    return (bits & BIT_CLOSED) ? "unreachable" : "timeout";
}

static bool send_key(const char *key)
{
    char msg[200];
    int len = snprintf(msg, sizeof(msg),
                       "{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"Click\","
                       "\"DataOfCmd\":\"%s\",\"Option\":\"false\",\"TypeOfRemote\":\"SendRemoteKey\"}}",
                       key);
    ESP_LOGI(TAG, "key %s", key);
    return esp_websocket_client_send_text(s_client, msg, len, pdMS_TO_TICKS(2000)) == len;
}

// --- Macros ---------------------------------------------------------------

typedef struct {
    const char *key;
    uint32_t delay_ms;  // wait after this key
} step_t;

#define MAX_STEPS (2 + MENU_MAX_ROWS + 2)

static int build_input_macro(int input, step_t *steps)
{
    int n = 0;
#ifdef TV_USE_CYCLE_MACRO
    steps[n++] = (step_t){"KEY_TV", CYCLE_TV_DELAY_MS};
    for (int i = 0; i < input; i++) {
        steps[n++] = (step_t){"KEY_HDMI", CYCLE_HDMI_DELAY_MS};
    }
#else
    // The Source menu's input column is TV, HDMI 1, HDMI 2. Up stops at TV
    // (no wrap) and Left exits the menu, so never press Left.
    steps[n++] = (step_t){"KEY_SOURCE", MENU_OPEN_DELAY_MS};
    for (int i = 0; i < MENU_MAX_ROWS; i++) {
        steps[n++] = (step_t){"KEY_UP", MENU_KEY_DELAY_MS};
    }
    for (int i = 0; i < input; i++) {
        steps[n++] = (step_t){"KEY_DOWN", MENU_KEY_DELAY_MS};
    }
    steps[n++] = (step_t){"KEY_ENTER", 0};
#endif
    return n;
}

static const char *ensure_open(void)
{
    if (ws_is_open()) {
        return "ok";
    }
    bool paired = s_token[0] != '\0';
    if (!paired) {
        ESP_LOGW(TAG, "not paired: accept the prompt on the TV");
    }
    return ws_open(paired ? TV_CONNECT_TIMEOUT_MS : TV_PAIR_TIMEOUT_MS);
}

static const char *run_steps(const step_t *steps, int n)
{
    const char *r = ensure_open();
    if (strcmp(r, "ok") != 0) {
        return r;
    }
    for (int i = 0; i < n; i++) {
        if (!send_key(steps[i].key)) {
            ws_close();
            return "send failed";
        }
        if (steps[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(steps[i].delay_ms));
        }
    }
    return "ok";
}

// --- Power ----------------------------------------------------------------

static void set_power(const char *state)
{
    status_t *st = status_lock();
    strlcpy(st->tv_power, state, sizeof(st->tv_power));
    status_unlock();
}

// Reads PowerState from http://TV_IP:8001/api/v2/: "on", "standby", or "off"
// when the TV doesn't answer at all. Also refreshed on every idle tick.
static const char *query_power(void)
{
    static char body[4096];
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/api/v2/", TV_IP, TV_REST_PORT);

    esp_http_client_config_t cfg = {.url = url, .timeout_ms = TV_POWER_QUERY_MS};
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    const char *state = "off";
    if (esp_http_client_open(http, 0) == ESP_OK && esp_http_client_fetch_headers(http) >= 0) {
        int len = 0, n;
        while (len < (int)sizeof(body) - 1 &&
               (n = esp_http_client_read(http, body + len, sizeof(body) - 1 - len)) > 0) {
            len += n;
        }
        body[len] = '\0';
        cJSON *root = cJSON_Parse(body);
        const cJSON *ps = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "device"), "PowerState");
        state = cJSON_IsString(ps) && strcmp(ps->valuestring, "on") == 0 ? "on" : "standby";
        cJSON_Delete(root);
    }
    esp_http_client_cleanup(http);
    return state;
}

// After a wake, put the TV on the lever's input (nothing in API mode). Gives
// up early if another command is queued: it was asked for later, so it wins.
static const char *switch_after_wake(void)
{
    int64_t t0 = esp_timer_get_time();
    const char *r = "timeout";
    while ((esp_timer_get_time() - t0) / 1000 < TV_WAKE_TIMEOUT_MS) {
        if (uxQueueMessagesWaiting(s_queue)) {
            return "wake, superseded";
        }
        if (strcmp(query_power(), "on") == 0) {
            set_power("on");
            r = s_token[0] ? ws_open(TV_CONNECT_TIMEOUT_MS) : "not paired";
            if (strcmp(r, "ok") == 0) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    uint32_t up_ms = (esp_timer_get_time() - t0) / 1000;
    if (strcmp(r, "ok") != 0) {
        ESP_LOGW(TAG, "TV not controllable %lu ms after wake: %s", (unsigned long)up_ms, r);
        return "wake, no connect";
    }
    ESP_LOGI(TAG, "TV up and connected %lu ms after wake", (unsigned long)up_ms);

    ctl_mode_t mode;
    status_t *st = status_lock();
    mode = st->mode;
    status_unlock();
    if (mode != MODE_HDMI1 && mode != MODE_HDMI2) {
        return "wake, api mode";
    }
    int input = mode == MODE_HDMI1 ? 1 : 2;

    // Freshly booted TVs drop keys for a moment
    vTaskDelay(pdMS_TO_TICKS(TV_WAKE_SETTLE_MS));
    if (uxQueueMessagesWaiting(s_queue)) {
        return "wake, superseded";
    }
    step_t steps[MAX_STEPS];
    int n = build_input_macro(input, steps);
    r = run_steps(steps, n);
    if (strcmp(r, "ok") != 0) {
        return "wake, input failed";
    }
    status_lock()->input = input;
    status_unlock();
    return input == 1 ? "wake, input 1" : "wake, input 2";
}

static const char *run_power(void)
{
    const char *state = query_power();
    ESP_LOGI(TAG, "TV power state: %s", state);
    set_power(state);

    if (strcmp(state, "on") == 0) {
        step_t step = {"KEY_POWER", 0};
        const char *r = run_steps(&step, 1);
        if (strcmp(r, "ok") == 0) {
            // The TV goes dark without closing the socket; drop it so later
            // commands see it as unreachable instead of sending into nothing
            ws_close();
            set_power("standby");
            return "off (KEY_POWER)";
        }
        return r;
    }
    esp_err_t err = eth_dhcps_send_wol();
    if (err == ESP_ERR_INVALID_STATE) {
        return "no TV MAC yet";
    }
    if (err != ESP_OK) {
        return "WOL send failed";
    }
    set_power("waking");  // until the next background check sees it on
    return switch_after_wake();
}

static void run_command(const tv_cmd_t *cmd)
{
    char what[24];
    const char *result;
    int64_t t0 = esp_timer_get_time();

    status_lock()->busy = true;
    status_unlock();

    if (cmd->type == CMD_INPUT) {
        snprintf(what, sizeof(what), "input %d", cmd->input);
        step_t steps[MAX_STEPS];
        int n = build_input_macro(cmd->input, steps);
        result = run_steps(steps, n);
        if (strcmp(result, "ok") == 0) {
            status_lock()->input = cmd->input;
            status_unlock();
        }
    } else if (cmd->type == CMD_KEY) {
        snprintf(what, sizeof(what), "key %.19s", cmd->key);
        step_t step = {cmd->key, 0};
        result = run_steps(&step, 1);
    } else if (cmd->type == CMD_POWER) {
        strlcpy(what, "power", sizeof(what));
        result = run_power();
    } else {
        strlcpy(what, "pair", sizeof(what));
        ws_close();
        token_save(NULL);
        ESP_LOGW(TAG, "pairing: accept the prompt on the TV within %d s", TV_PAIR_TIMEOUT_MS / 1000);
        result = ws_open(TV_PAIR_TIMEOUT_MS);
    }

    uint32_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "%s from %s: %s (%lu ms)", what, status_source_name(cmd->source), result, (unsigned long)ms);
    cmd_log_add(cmd->source, what, result, ms);

    status_lock()->busy = false;
    status_unlock();
}

static void tv_task(void *arg)
{
    for (;;) {
        tv_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(TV_RECONNECT_MS))) {
            run_command(&cmd);
        } else {
            const char *power = query_power();
            set_power(power);
            if (s_token[0] && !ws_is_open() && strcmp(power, "on") == 0) {
                // Keep a paired connection warm so switches skip the TLS handshake
                ws_open(TV_CONNECT_TIMEOUT_MS);
            }
        }
    }
}

// --- Public ---------------------------------------------------------------

static void enqueue(const tv_cmd_t *cmd)
{
    xQueueOverwrite(s_queue, cmd);
}

void tv_request_input(int input, cmd_source_t source)
{
    tv_cmd_t cmd = {.type = CMD_INPUT, .source = source, .input = input};
    enqueue(&cmd);
}

void tv_request_key(const char *key, cmd_source_t source)
{
    tv_cmd_t cmd = {.type = CMD_KEY, .source = source};
    strlcpy(cmd.key, key, sizeof(cmd.key));
    enqueue(&cmd);
}

void tv_request_pair(cmd_source_t source)
{
    tv_cmd_t cmd = {.type = CMD_PAIR, .source = source};
    enqueue(&cmd);
}

void tv_request_power(cmd_source_t source)
{
    tv_cmd_t cmd = {.type = CMD_POWER, .source = source};
    enqueue(&cmd);
}

void tv_start(void)
{
    s_queue = xQueueCreate(1, sizeof(tv_cmd_t));
    s_events = xEventGroupCreate();
    token_load();
    ESP_LOGI(TAG, "TV at %s, %s", TV_IP, s_token[0] ? "paired" : "not paired");
    xTaskCreate(tv_task, "tv", 6144, NULL, 5, NULL);
}
