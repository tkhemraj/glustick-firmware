#include "voice_jbuf.h"
#include <string.h>

#define JBUF_SIZE     11520   // 4 voice packets (4 * 2880 samples)
#define PREBUF_THRESH 2880    // 1 packet (~360 ms) before playout starts

// Free-running indices (masked only for addressing). Unsigned (head - tail)
// gives the correct level across wrap for an SPSC ring where level <= SIZE.
static volatile size_t head = 0;
static volatile size_t tail = 0;
static volatile bool   playing = false;
static int16_t         jbuf[JBUF_SIZE];

void jbuf_reset() {
    head = 0;
    tail = 0;
    playing = false;
}

void jbuf_push(const int16_t *pcm, size_t n) {
    size_t level = head - tail;
    size_t freesp = JBUF_SIZE - level;
    if (n > freesp) n = freesp;      // full: drop the NEW samples that don't fit (SPSC-safe)
    if (n == 0) return;

    size_t idx = head % JBUF_SIZE;
    if (idx + n > JBUF_SIZE) {
        size_t first = JBUF_SIZE - idx;
        memcpy(&jbuf[idx], pcm, first * sizeof(int16_t));
        memcpy(jbuf, &pcm[first], (n - first) * sizeof(int16_t));
    } else {
        memcpy(&jbuf[idx], pcm, n * sizeof(int16_t));
    }
    head += n;

    if (!playing && (head - tail) >= PREBUF_THRESH) playing = true;
}

size_t jbuf_pop(int16_t *out, size_t n) {
    if (!playing) {                   // still prebuffering (or drained) -> silence
        memset(out, 0, n * sizeof(int16_t));
        return n;
    }

    size_t level   = head - tail;
    size_t to_read = (level < n) ? level : n;

    size_t idx = tail % JBUF_SIZE;
    if (idx + to_read > JBUF_SIZE) {
        size_t first = JBUF_SIZE - idx;
        memcpy(out, &jbuf[idx], first * sizeof(int16_t));
        memcpy(&out[first], jbuf, (to_read - first) * sizeof(int16_t));
    } else {
        memcpy(out, &jbuf[idx], to_read * sizeof(int16_t));
    }
    tail += to_read;

    if (to_read < n)                  // partial underrun -> zero-fill the rest (no click)
        memset(&out[to_read], 0, (n - to_read) * sizeof(int16_t));

    if (tail == head) playing = false; // fully drained -> re-prime on next burst
    return n;
}

bool   jbuf_playing() { return playing; }
size_t jbuf_level()   { return head - tail; }
