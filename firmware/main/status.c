#include "status.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static status_t s_status = {.mode = MODE_API, .tv_power = "unknown"};

static cmd_log_entry_t s_log[CMD_LOG_LEN];
static int s_log_next;
static int s_log_count;

void status_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

status_t *status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_status;
}

void status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void status_get(status_t *out)
{
    *out = *status_lock();
    status_unlock();
}

const char *status_mode_name(ctl_mode_t mode)
{
    switch (mode) {
    case MODE_HDMI1: return "hdmi1";
    case MODE_API:   return "api";
    case MODE_HDMI2: return "hdmi2";
    default:         return "fault";
    }
}

const char *status_source_name(cmd_source_t source)
{
    switch (source) {
    case SRC_SWITCH: return "switch";
    case SRC_BUTTON: return "button";
    default:         return "api";
    }
}

void cmd_log_add(cmd_source_t source, const char *what, const char *result, uint32_t duration_ms)
{
    status_lock();
    cmd_log_entry_t *e = &s_log[s_log_next];
    e->at_ms = esp_timer_get_time() / 1000;
    e->source = source;
    strlcpy(e->what, what, sizeof(e->what));
    strlcpy(e->result, result, sizeof(e->result));
    e->duration_ms = duration_ms;
    s_log_next = (s_log_next + 1) % CMD_LOG_LEN;
    if (s_log_count < CMD_LOG_LEN) {
        s_log_count++;
    }
    status_unlock();
}

int cmd_log_get(cmd_log_entry_t *out)
{
    status_lock();
    int first = (s_log_next - s_log_count + CMD_LOG_LEN) % CMD_LOG_LEN;
    for (int i = 0; i < s_log_count; i++) {
        out[i] = s_log[(first + i) % CMD_LOG_LEN];
    }
    int n = s_log_count;
    status_unlock();
    return n;
}
