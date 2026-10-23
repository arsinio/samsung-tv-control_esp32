// Shared device state, guarded by one mutex.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MODE_HDMI1,  // switch owns the TV, API locked out
    MODE_API,    // center: API owns the TV
    MODE_HDMI2,
    MODE_FAULT,  // both throws read closed; locked, no action
} ctl_mode_t;

typedef enum {
    SRC_SWITCH,
    SRC_API,
} cmd_source_t;

typedef struct {
    ctl_mode_t mode;
    int input;               // last input commanded (1 or 2), 0 if none yet
    bool busy;               // a TV macro is running

    bool eth_link;
    bool dhcp_serving;
    bool dhcp_rogue;         // another DHCP server answered on the TV link
    bool subnet_conflict;    // Wi-Fi subnet overlaps the TV link
    bool tv_leased;          // a lease was handed out since boot
    uint8_t tv_mac[6];

    bool tv_connected;
    bool tv_paired;          // a token is stored

    bool wifi_connected;
    char wifi_ip[16];
} status_t;

// Command log entry, newest last
typedef struct {
    int64_t at_ms;           // ms since boot
    cmd_source_t source;
    char what[24];           // "input 2", "key KEY_INFO", "pair"
    char result[24];         // "ok", "not connected", ...
    uint32_t duration_ms;
} cmd_log_entry_t;

#define CMD_LOG_LEN 16

void status_init(void);

// Lock, then read or modify the returned struct, then unlock.
status_t *status_lock(void);
void status_unlock(void);

// Copy of the current state.
void status_get(status_t *out);

const char *status_mode_name(ctl_mode_t mode);
const char *status_source_name(cmd_source_t source);

void cmd_log_add(cmd_source_t source, const char *what, const char *result, uint32_t duration_ms);
// Copies up to CMD_LOG_LEN entries, oldest first; returns the count.
int cmd_log_get(cmd_log_entry_t *out);
