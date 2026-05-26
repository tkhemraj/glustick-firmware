#pragma once
#include <Arduino.h>
#include "config.h"

#define MSG_BODY_MAX 220  // ~5 chunks × 44 bytes, enough for any short message

typedef struct {
    uint16_t id;
    char     body[MSG_BODY_MAX + 1];
    bool     outbound;  // true = waiting to send uplink; false = received downlink
    bool     acked;
} QueuedMessage;

// NVS-backed queue. Survives deep sleep and power cycles.
void     queue_init();
bool     queue_push_outbound(const char *body);          // returns false if full
bool     queue_push_inbound(uint16_t id, const char *body);
bool     queue_pop_outbound(QueuedMessage *out);         // returns false if empty
void     queue_mark_acked(uint16_t id);
uint8_t  queue_outbound_count();
void     queue_flush_acked();
uint16_t queue_next_msg_id();
