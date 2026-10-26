// Samsung TV remote-control client. One task owns the WebSocket and runs
// commands one at a time; a new request replaces any queued one (latest wins).
#pragma once

#include "status.h"

void tv_start(void);

void tv_request_input(int input, cmd_source_t source);
void tv_request_key(const char *key, cmd_source_t source);
// Drops the stored token and reconnects, so the TV shows its Allow prompt.
void tv_request_pair(cmd_source_t source);
// Toggles power: KEY_POWER if the TV reports it's on, otherwise Wake-on-LAN.
void tv_request_power(cmd_source_t source);
