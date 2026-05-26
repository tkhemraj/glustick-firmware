#include "frame.h"
#include <string.h>

size_t frame_encode(const Frame *f, uint8_t *buf) {
    buf[0] = f->version;
    buf[1] = f->type;
    buf[2] = (uint8_t)(f->msg_id & 0xFF);
    buf[3] = (uint8_t)(f->msg_id >> 8);
    buf[4] = f->chunk_idx;
    buf[5] = f->chunk_total;
    memcpy(buf + FRAME_HEADER_SIZE, f->data, f->data_len);
    return FRAME_HEADER_SIZE + f->data_len;
}

bool frame_decode(const uint8_t *buf, size_t len, Frame *f) {
    if (len < FRAME_HEADER_SIZE) return false;
    f->version     = buf[0];
    f->type        = buf[1];
    f->msg_id      = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    f->chunk_idx   = buf[4];
    f->chunk_total = buf[5];
    f->data_len    = (uint8_t)(len - FRAME_HEADER_SIZE);
    if (f->data_len > CHUNK_MAX_DATA) f->data_len = CHUNK_MAX_DATA;
    memcpy(f->data, buf + FRAME_HEADER_SIZE, f->data_len);
    return true;
}

void frame_make_ping(uint16_t msg_id, Frame *f) {
    f->version     = FRAME_VERSION;
    f->type        = FRAME_TYPE_PING;
    f->msg_id      = msg_id;
    f->chunk_idx   = 0;
    f->chunk_total = 1;
    f->data_len    = 0;
}

void frame_make_ack(uint16_t msg_id, Frame *f) {
    f->version     = FRAME_VERSION;
    f->type        = FRAME_TYPE_ACK;
    f->msg_id      = msg_id;
    f->chunk_idx   = 0;
    f->chunk_total = 1;
    f->data_len    = 0;
}
