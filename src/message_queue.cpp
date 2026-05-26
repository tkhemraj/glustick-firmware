#include "message_queue.h"
#include <Preferences.h>

static Preferences prefs;
static QueuedMessage queue[MSG_QUEUE_MAX];
static uint8_t  q_count   = 0;
static uint16_t next_id   = 1;

void queue_init() {
    prefs.begin("gsf_q", false);
    q_count = prefs.getUChar("count", 0);
    next_id = prefs.getUShort("next_id", 1);
    if (q_count > MSG_QUEUE_MAX) q_count = 0;

    for (uint8_t i = 0; i < q_count; i++) {
        char key[12];
        snprintf(key, sizeof(key), "m%u_id", i);
        queue[i].id = prefs.getUShort(key, 0);
        snprintf(key, sizeof(key), "m%u_ob", i);
        queue[i].outbound = prefs.getBool(key, true);
        snprintf(key, sizeof(key), "m%u_ack", i);
        queue[i].acked = prefs.getBool(key, false);
        snprintf(key, sizeof(key), "m%u_body", i);
        prefs.getString(key, queue[i].body, MSG_BODY_MAX);
    }
}

static void persist_slot(uint8_t i) {
    char key[16];
    snprintf(key, sizeof(key), "m%u_id", i);   prefs.putUShort(key, queue[i].id);
    snprintf(key, sizeof(key), "m%u_ob", i);   prefs.putBool(key, queue[i].outbound);
    snprintf(key, sizeof(key), "m%u_ack", i);  prefs.putBool(key, queue[i].acked);
    snprintf(key, sizeof(key), "m%u_body", i); prefs.putString(key, queue[i].body);
}

bool queue_push_outbound(const char *body) {
    if (q_count >= MSG_QUEUE_MAX) return false;
    queue[q_count].id       = next_id++;
    queue[q_count].outbound = true;
    queue[q_count].acked    = false;
    strncpy(queue[q_count].body, body, MSG_BODY_MAX);
    queue[q_count].body[MSG_BODY_MAX] = '\0';
    persist_slot(q_count);
    q_count++;
    prefs.putUChar("count", q_count);
    prefs.putUShort("next_id", next_id);
    return true;
}

bool queue_push_inbound(uint16_t id, const char *body) {
    if (q_count >= MSG_QUEUE_MAX) return false;
    queue[q_count].id       = id;
    queue[q_count].outbound = false;
    queue[q_count].acked    = true;
    strncpy(queue[q_count].body, body, MSG_BODY_MAX);
    queue[q_count].body[MSG_BODY_MAX] = '\0';
    persist_slot(q_count);
    q_count++;
    prefs.putUChar("count", q_count);
    return true;
}

bool queue_pop_outbound(QueuedMessage *out) {
    for (uint8_t i = 0; i < q_count; i++) {
        if (queue[i].outbound && !queue[i].acked) {
            *out = queue[i];
            return true;
        }
    }
    return false;
}

void queue_mark_acked(uint16_t id) {
    for (uint8_t i = 0; i < q_count; i++) {
        if (queue[i].id == id) {
            queue[i].acked = true;
            char key[16];
            snprintf(key, sizeof(key), "m%u_ack", i);
            prefs.putBool(key, true);
            return;
        }
    }
}

uint8_t queue_outbound_count() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < q_count; i++)
        if (queue[i].outbound && !queue[i].acked) n++;
    return n;
}

void queue_flush_acked() {
    uint8_t new_count = 0;
    QueuedMessage tmp[MSG_QUEUE_MAX];
    for (uint8_t i = 0; i < q_count; i++) {
        if (!queue[i].acked) tmp[new_count++] = queue[i];
    }
    memcpy(queue, tmp, sizeof(QueuedMessage) * new_count);
    q_count = new_count;
    prefs.putUChar("count", q_count);
    for (uint8_t i = 0; i < q_count; i++) persist_slot(i);
}

uint16_t queue_next_msg_id() {
    return next_id;
}
