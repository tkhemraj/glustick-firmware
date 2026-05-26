#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"

// Wire format:
//   [0]   version   = 1
//   [1]   type      = 0 msg | 1 ack | 2 ping
//   [2-3] msgID     uint16 LE
//   [4]   chunkIdx
//   [5]   chunkTotal
//   [6+]  data      max 44 bytes

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t msg_id;
    uint8_t  chunk_idx;
    uint8_t  chunk_total;
    uint8_t  data[CHUNK_MAX_DATA];
    uint8_t  data_len;
} Frame;

// Encode frame into buf (must be >= LORA_MAX_PAYLOAD bytes). Returns encoded length.
size_t frame_encode(const Frame *f, uint8_t *buf);

// Decode buf into f. Returns true on success.
bool frame_decode(const uint8_t *buf, size_t len, Frame *f);

// Build a ping frame.
void frame_make_ping(uint16_t msg_id, Frame *f);

// Build an ack frame.
void frame_make_ack(uint16_t msg_id, Frame *f);
