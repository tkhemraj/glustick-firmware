#include "display.h"
#include "config.h"

// ═════════════════════════════════════════════════════════════════════════════
// T-Deck — ST7789 320×240 IPS — production chat UI
// ═════════════════════════════════════════════════════════════════════════════
#ifdef BOARD_TDECK

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <qrcode.h>
#include "fonts/FreeSansBold10pt7b.h"

static constexpr int8_t DISP_DC   = 11;
static constexpr int8_t DISP_CS   = 12;
static constexpr int8_t DISP_SCK  = 40;
static constexpr int8_t DISP_MOSI = 41;
static constexpr int8_t DISP_MISO = 38;
static constexpr int8_t DISP_RST  = -1;

static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX     *gfx  = nullptr;

// ── Purple palette (RGB565) ──────────────────────────────────────────────────
static constexpr uint16_t C_BG         = 0x0844u;  // #0D0820 deep purple-black
static constexpr uint16_t C_HDR        = 0x2048u;  // #200845 dark header
static constexpr uint16_t C_ACCENT     = 0x9A79u;  // #9D4DC9 vibrant purple
static constexpr uint16_t C_ACCENT2    = 0xC43Fu;  // #C084FC lavender
static constexpr uint16_t C_TEXT       = 0xFFFFu;
static constexpr uint16_t C_DIM        = 0x7B51u;  // muted purple-grey
static constexpr uint16_t C_WARN       = 0xFC60u;  // amber
static constexpr uint16_t C_ERR        = 0xF800u;
static constexpr uint16_t C_OK         = 0x07E0u;
static constexpr uint16_t C_BUBBLE_IN  = 0x1867u;  // dark purple incoming
static constexpr uint16_t C_BUBBLE_OUT = 0x28ABu;  // medium purple outgoing
static constexpr uint16_t C_BLACK      = 0x0000u;
static constexpr uint16_t C_WHITE      = 0xFFFFu;

static const int16_t W = DISPLAY_WIDTH;   // 320
static const int16_t H = DISPLAY_HEIGHT;  // 240

// ── Layout ───────────────────────────────────────────────────────────────────
static constexpr int16_t STAT_H = 28;          // status bar at top
static constexpr int16_t COMP_H = 28;          // compose bar at bottom
static constexpr int16_t MSG_Y0 = STAT_H;      // = 28, top of chat area
static constexpr int16_t MSG_Y1 = H - COMP_H;  // = 212, bottom of chat area

// ── Message ring buffer ───────────────────────────────────────────────────────
static constexpr int CHAT_MSGS_MAX = 16;
static constexpr int CHAT_BODY_MAX = 128;
static constexpr int CHAT_FROM_MAX = 20;

struct ChatMsg { char body[CHAT_BODY_MAX]; char from[CHAT_FROM_MAX]; bool outgoing; };
static ChatMsg s_msgs[CHAT_MSGS_MAX];
static int     s_msg_total = 0;  // total inserted (wraps mod CHAT_MSGS_MAX)

static inline int chat_count() {
    return s_msg_total < CHAT_MSGS_MAX ? s_msg_total : CHAT_MSGS_MAX;
}
// msg_at(0)=oldest visible, msg_at(count-1)=newest
static inline const ChatMsg *msg_at(int i) {
    int n = chat_count();
    if (i < 0 || i >= n) return nullptr;
    int base = s_msg_total >= CHAT_MSGS_MAX ? s_msg_total % CHAT_MSGS_MAX : 0;
    return &s_msgs[(base + i) % CHAT_MSGS_MAX];
}

// ── Cached device state (used by partial redraws) ────────────────────────────
static char s_dev_name[32]            = "Glustick";
static int  s_rssi_val                = 0;
static int  s_bat_val                 = 100;
static int  s_wifi_rssi               = 0;     // 0=not connected, negative=connected
static bool s_mqtt_conn               = false;
static char s_compose_buf[COMPOSE_BUF_MAX] = {};
static int  s_scroll                  = 0;  // 0=latest at bottom, +n=scrolled back

// FreeSansBold10pt7b: baseline is ~11 px below cell top
static constexpr int16_t FB = 11;

// ── Font / colour helpers ────────────────────────────────────────────────────
static void sans() { gfx->setFont(&FreeSansBold10pt7b); }
static void dflt() { gfx->setFont(nullptr); }

static void tleft(const char *s, int16_t x, int16_t row_top,
                  uint16_t col, uint16_t bg, bool use_sans = true) {
    if (use_sans) sans(); else dflt();
    gfx->setTextColor(col, bg);
    gfx->setCursor(x, row_top + FB);
    gfx->print(s);
}

static void tcenter(const char *s, int16_t rx, int16_t rw, int16_t row_top,
                    uint16_t col, uint16_t bg, bool use_sans = true) {
    if (use_sans) sans(); else dflt();
    gfx->setTextColor(col, bg);
    int16_t x1, y1; uint16_t tw, th;
    gfx->getTextBounds(s, 0, row_top + FB, &x1, &y1, &tw, &th);
    gfx->setCursor(rx + (rw - (int16_t)tw) / 2 - x1, row_top + FB);
    gfx->print(s);
}
static void tcenter(const char *s, int16_t row_top,
                    uint16_t col, uint16_t bg, bool use_sans = true) {
    tcenter(s, 0, W, row_top, col, bg, use_sans);
}

// ── Word-wrap renderer (returns y after last line) ───────────────────────────
static int16_t draw_wrapped(const char *text,
                             int16_t x, int16_t y,
                             int16_t max_w, int16_t max_y,
                             uint16_t col, uint16_t bg) {
    sans();
    gfx->setTextColor(col, bg);
    const int16_t LINE_H = 16;
    char word[64];
    int ti = 0, len = (int)strlen(text);
    int16_t cx = x, cy = y + FB;

    while (ti < len && cy - FB + LINE_H <= max_y) {
        int wi = 0;
        while (ti < len && text[ti] != ' ' && text[ti] != '\n' && wi < 62)
            word[wi++] = text[ti++];
        word[wi] = '\0';
        bool nl = (wi > 0 && ti < len && text[ti] == '\n');
        if (ti < len && (text[ti] == ' ' || text[ti] == '\n')) ti++;
        if (wi == 0) { cy += LINE_H; cx = x; continue; }

        int16_t x1, y1; uint16_t tw, th;
        gfx->getTextBounds(word, cx, cy, &x1, &y1, &tw, &th);
        if (cx > x && cx + (int16_t)tw > x + max_w) {
            cy += LINE_H; cx = x;
            gfx->getTextBounds(word, cx, cy, &x1, &y1, &tw, &th);
        }
        gfx->setCursor(cx, cy); gfx->print(word);
        cx += (int16_t)tw + 5;
        if (nl) { cy += LINE_H; cx = x; }
    }
    return cy - FB;
}

// Count lines needed to word-wrap text into max_w (uses FreeSansBold metrics)
static int count_lines(const char *text, int16_t max_w) {
    sans();
    int lines = 1;
    int16_t cx = 0;
    char word[64];
    int ti = 0, len = (int)strlen(text);

    while (ti < len) {
        int wi = 0;
        while (ti < len && text[ti] != ' ' && text[ti] != '\n' && wi < 62)
            word[wi++] = text[ti++];
        word[wi] = '\0';
        bool nl = (ti < len && text[ti] == '\n');
        if (ti < len) ti++;
        if (wi == 0) { if (nl) { lines++; cx = 0; } continue; }

        int16_t x1, y1; uint16_t tw, th;
        gfx->getTextBounds(word, cx, 20, &x1, &y1, &tw, &th);
        if (cx > 0 && cx + (int16_t)tw > max_w) {
            lines++; cx = 0;
            gfx->getTextBounds(word, 0, 20, &x1, &y1, &tw, &th);
        }
        cx += (int16_t)tw + 5;
        if (nl) { lines++; cx = 0; }
    }
    return lines;
}

static int16_t bubble_h(const char *text, int16_t text_max_w) {
    return (int16_t)(count_lines(text, text_max_w) * 16 + 16);
}

// ── Status bar (top 28 px) ───────────────────────────────────────────────────
// Layout from right edge (W=320):
//   LoRa bars (4): W-30 .. W-4    (always)
//   WiFi bars (3): W-48 .. W-34   (colour = connected; dim = not)
//   MQTT dot:      cx=W-57        (green = connected; dim = not)
//   Battery:       W-90 .. W-68   (with 2px nub at W-68)
static void draw_status_bar() {
    gfx->fillRect(0, 0, W, STAT_H, C_HDR);
    gfx->drawFastHLine(0, STAT_H - 1, W, C_ACCENT);
    const int cy = STAT_H / 2;
    const int ty = (STAT_H - 8) / 2;  // y for 8px-tall default font

    // Device name (left)
    sans();
    gfx->setTextColor(C_ACCENT2, C_HDR);
    gfx->setCursor(10, STAT_H - 7);
    gfx->print(s_dev_name);

    dflt(); gfx->setTextSize(1);

    // ── right side, working left from screen edge ─────────────────────────────

    // LoRa signal bars — 4 bars, ends at W-8
    bool lora_linked = (s_rssi_val != 0);
    int lora_bars = !lora_linked ? 0 :
                    s_rssi_val >= -60 ? 4 : s_rssi_val >= -70 ? 3 :
                    s_rssi_val >= -80 ? 2 : 1;
    static const int8_t BH[4] = { 4, 6, 8, 10 };
    for (int i = 0; i < 4; i++) {
        int bbx = (W - 34) + i * 7;
        int bby = STAT_H - 3 - BH[i];
        gfx->fillRect(bbx, bby, 5, BH[i], i < lora_bars ? C_OK : C_DIM);
    }

    // "LR" label just left of LoRa bars — green when a peer has been heard
    uint16_t lr_col = lora_linked ? C_OK : C_DIM;
    gfx->setTextColor(lr_col, C_HDR);
    gfx->setCursor(W - 50, ty);
    gfx->print("LR");

    // WiFi/MQTT dot — left of "LR"
    uint16_t wf_col = s_mqtt_conn ? C_OK : C_DIM;
    gfx->fillCircle(W - 60, cy, 4, C_HDR);   // erase
    gfx->fillCircle(W - 60, cy, 3, wf_col);

    // "WF" label just left of MQTT dot — green when broker connected
    gfx->setTextColor(wf_col, C_HDR);
    gfx->setCursor(W - 78, ty);
    gfx->print("WF");

    // Battery icon — 22×10 px at x=W-106, with 2px tip
    char bat_pct_str[5];
    snprintf(bat_pct_str, sizeof(bat_pct_str), "%d%%", s_bat_val);
    uint16_t bat_col = s_bat_val > 25 ? C_OK : (s_bat_val > 10 ? C_WARN : C_ERR);
    const int bx = W - 106, by = (STAT_H - 10) / 2;
    gfx->drawRect(bx, by, 22, 10, C_DIM);
    int bfill = (s_bat_val * 20) / 100;
    if (bfill > 0) gfx->fillRect(bx + 1, by + 1, bfill, 8, bat_col);
    gfx->fillRect(bx + 22, by + 3, 2, 4, C_DIM);

    // Battery % — right-aligned, ends 3px left of battery icon
    int pct_chars = (int)strlen(bat_pct_str);
    gfx->setTextColor(bat_col, C_HDR);
    gfx->setCursor(bx - 3 - pct_chars * 6, ty);
    gfx->print(bat_pct_str);
}

// ── Compose bar (bottom 28 px) ───────────────────────────────────────────────
static void draw_compose_bar() {
    const int16_t y = MSG_Y1;
    gfx->fillRect(0, y, W, COMP_H, C_HDR);
    gfx->drawFastHLine(0, y, W, C_ACCENT);

    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_ACCENT2, C_HDR);
    gfx->setCursor(8, y + 10);
    gfx->print(">");

    const char *draft = s_compose_buf;
    int dlen = (int)strlen(draft);
    const char *show = dlen > 46 ? draft + dlen - 46 : draft;
    gfx->setTextColor(C_TEXT, C_HDR);
    const int tx = 18;
    gfx->setCursor(tx, y + 10);
    gfx->print(show);

    // Block cursor
    int cx = tx + (int)strlen(show) * 6;
    if (cx < W - 8) gfx->fillRect(cx, y + 4, 2, 14, C_ACCENT2);
}

// ── Chat message area (MSG_Y0..MSG_Y1) ───────────────────────────────────────
static void draw_message_area() {
    gfx->fillRect(0, MSG_Y0, W, MSG_Y1 - MSG_Y0, C_BG);

    int n = chat_count();
    if (n == 0) {
        dflt(); gfx->setTextSize(1);
        const int16_t my = (MSG_Y0 + MSG_Y1) / 2;
        gfx->setTextColor(C_DIM, C_BG);
        tcenter("No messages yet.", my - 8, C_DIM, C_BG, false);
        tcenter("Type below and press Enter.", my + 8, C_DIM, C_BG, false);
        return;
    }

    static constexpr int16_t GAP    = 6;    // gap between bubbles
    static constexpr int16_t PADH   = 10;   // horizontal text padding inside bubble
    static constexpr int16_t MARGIN = 10;   // screen edge margin
    static constexpr int16_t BUB_W  = 230;  // fixed bubble width
    static constexpr int16_t TXW    = BUB_W - PADH * 2;

    // Render from bottom up — newest visible message at bottom
    // s_scroll > 0 means the user scrolled back; offset the starting index.
    int start = n - 1 - s_scroll;
    if (start < 0) start = 0;

    int16_t y = MSG_Y1 - GAP;
    for (int i = start; i >= 0; i--) {
        const ChatMsg *m = msg_at(i);
        if (!m) break;

        int16_t bh = bubble_h(m->body, TXW);
        y -= bh;
        if (y < MSG_Y0 + GAP) break;  // no room for more bubbles above

        int16_t bx       = m->outgoing ? (W - MARGIN - BUB_W) : MARGIN;
        uint16_t bg_col  = m->outgoing ? C_BUBBLE_OUT : C_BUBBLE_IN;
        uint16_t bdr_col = m->outgoing ? C_ACCENT     : C_DIM;

        gfx->fillRoundRect(bx, y, BUB_W, bh, 8, bg_col);
        gfx->drawRoundRect(bx, y, BUB_W, bh, 8, bdr_col);
        draw_wrapped(m->body, bx + PADH, y + 8, TXW, y + bh - 8, C_TEXT, bg_col);

        y -= GAP;
    }
}

// ── Power-on latch (runs before setup()) ────────────────────────────────────
extern "C" void initVariant() {
    pinMode(TDECK_POWERON_PIN, OUTPUT); digitalWrite(TDECK_POWERON_PIN, HIGH);
    pinMode(TFT_BL, OUTPUT);           digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
}

// ── Public API ───────────────────────────────────────────────────────────────

void display_init() {
    pinMode(TDECK_POWERON_PIN, OUTPUT); digitalWrite(TDECK_POWERON_PIN, HIGH);
    pinMode(TFT_BL, OUTPUT);           digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
    pinMode(TDECK_SD_CS, OUTPUT);      digitalWrite(TDECK_SD_CS, HIGH);
    pinMode(PIN_LMIC_NSS, OUTPUT);     digitalWrite(PIN_LMIC_NSS, HIGH);

    s_bus = new Arduino_ESP32SPI(DISP_DC, DISP_CS, DISP_SCK, DISP_MOSI, DISP_MISO);
    gfx  = new Arduino_ST7789(s_bus, DISP_RST, 1, true, 240, 320);
    gfx->begin(40000000UL);
    gfx->fillScreen(C_BG);
}

void display_show_boot() {
    gfx->fillScreen(C_BG);

    // Logo box
    gfx->fillRect(0, 40, W, 2, C_ACCENT);
    gfx->fillRect(0, 40, 4, 80, C_ACCENT);
    gfx->fillRect(W - 4, 40, 4, 80, C_ACCENT);
    gfx->fillRect(0, 120, W, 2, C_ACCENT);

    sans();
    gfx->setTextColor(0x500Bu, C_BG); gfx->setCursor(47, 93); gfx->print("Glustick");
    gfx->setTextColor(C_ACCENT2, C_BG); gfx->setCursor(45, 91); gfx->print("Glustick");

    gfx->fillRoundRect(W/2 - 24, 104, 48, 14, 4, C_HDR);
    tcenter(FW_VERSION, 105, C_ACCENT, C_HDR, false);

    gfx->drawFastHLine(40, 134, W - 80, C_HDR);

    sans();
    tcenter("Elysian v0.1", 168, C_DIM, C_BG);
}

void display_show_provisioning(const char * /*ble_name*/) {
    gfx->fillScreen(C_BG);
    draw_status_bar();
    tcenter("Provisioning...", (MSG_Y0 + MSG_Y1) / 2, C_DIM, C_BG);
}

// Full redraw: updates cached state, then redraws all three zones.
// No fillScreen — each zone fills its own area, so there's no flash.
void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t /*pending*/) {
    strncpy(s_dev_name, kid_name, sizeof(s_dev_name) - 1);
    s_rssi_val = rssi;
    s_bat_val  = battery_pct;
    draw_status_bar();
    draw_message_area();
    draw_compose_bar();
}

// Restore just the status bar — call this after display_show_sending() completes.
void display_refresh_status() { draw_status_bar(); }

// Add to chat history and redraw only the message area (status+compose untouched)
void display_push_message(const char *from, const char *body, bool outgoing) {
    int idx = s_msg_total % CHAT_MSGS_MAX;
    strncpy(s_msgs[idx].body, body, CHAT_BODY_MAX - 1);
    strncpy(s_msgs[idx].from, from, CHAT_FROM_MAX - 1);
    s_msgs[idx].body[CHAT_BODY_MAX - 1] = '\0';
    s_msgs[idx].from[CHAT_FROM_MAX - 1] = '\0';
    s_msgs[idx].outgoing = outgoing;
    s_msg_total++;
    s_scroll = 0;  // snap to latest on new message
    draw_message_area();
}

void display_scroll(int delta) {
    int n = chat_count();
    if (n == 0) return;
    s_scroll += delta;
    if (s_scroll < 0)       s_scroll = 0;
    if (s_scroll >= n)      s_scroll = n - 1;
    draw_message_area();
}

void display_touch(int16_t x, int16_t y) {
    // Tap in compose bar: nothing extra needed — compose is always active.
    // Tap in message area: snap scroll to latest (shows newest messages).
    if (y >= MSG_Y1) return;  // below compose
    if (y >= MSG_Y0 && y < MSG_Y1) {
        if (s_scroll != 0) { s_scroll = 0; draw_message_area(); }
    }
}

// Legacy: still works — routes into the persistent buffer
void display_show_message(const char *from, const char *body) {
    display_push_message(from, body, false);
}

// Only redraws the compose bar
void display_show_compose(const char *draft) {
    strncpy(s_compose_buf, draft, sizeof(s_compose_buf) - 1);
    s_compose_buf[sizeof(s_compose_buf) - 1] = '\0';
    draw_compose_bar();
}

// Briefly updates the status bar to show "Sending..." without clearing chat
void display_show_sending(uint8_t /*pending*/) {
    gfx->fillRect(0, 0, W, STAT_H, C_HDR);
    gfx->drawFastHLine(0, STAT_H - 1, W, C_ACCENT);
    sans(); gfx->setTextColor(C_WARN, C_HDR);
    gfx->setCursor(10, STAT_H - 7); gfx->print("Sending...");
}

// Shows which paths carried the outgoing message — call after lora_send(),
// then delay briefly, then display_refresh_status() to restore normal bar.
void display_show_sent_path(bool via_lora, bool via_wifi) {
    gfx->fillRect(0, 0, W, STAT_H, C_HDR);
    gfx->drawFastHLine(0, STAT_H - 1, W, C_ACCENT);
    sans();
    if (via_lora && via_wifi) {
        gfx->setTextColor(C_OK, C_HDR);
        gfx->setCursor(10, STAT_H - 7); gfx->print("Sent  Radio + WiFi");
    } else if (via_lora) {
        gfx->setTextColor(C_OK, C_HDR);
        gfx->setCursor(10, STAT_H - 7); gfx->print("Sent  Radio only");
    } else if (via_wifi) {
        gfx->setTextColor(C_WARN, C_HDR);
        gfx->setCursor(10, STAT_H - 7); gfx->print("Sent  WiFi only");
    } else {
        gfx->setTextColor(C_ERR, C_HDR);
        gfx->setCursor(10, STAT_H - 7); gfx->print("Send failed");
    }
}

void display_show_joining() {
    gfx->fillScreen(C_BG);
    draw_status_bar();
    const int16_t cy = (MSG_Y0 + MSG_Y1) / 2;
    tcenter("Connecting...", cy - 16, C_TEXT, C_BG);
    tcenter("This may take a moment.", cy + 4, C_DIM, C_BG);
}

void display_show_wifi_sync() {
    gfx->fillScreen(C_BG);
    draw_status_bar();
    const int16_t cy = (MSG_Y0 + MSG_Y1) / 2;
    tcenter("WiFi connected.", cy - 16, C_ACCENT2, C_BG);
    tcenter("Syncing messages...", cy + 4, C_DIM, C_BG);
}

void display_show_error(const char *msg) {
    gfx->fillScreen(C_BG);
    gfx->fillRect(0, 0, W, STAT_H, C_ERR);
    gfx->drawFastHLine(0, STAT_H, W, 0xFC00u);
    sans(); gfx->setTextColor(C_WHITE, C_ERR);
    gfx->setCursor(10, STAT_H - 7); gfx->print("ERROR");
    draw_wrapped(msg, 12, STAT_H + 8, W - 24, H - 20, C_TEXT, C_BG);
}

void display_set_lora_rssi(int rssi) { s_rssi_val = rssi; }

void display_set_wifi_status(int wifi_rssi, bool mqtt_connected) {
    s_wifi_rssi = wifi_rssi;
    s_mqtt_conn = mqtt_connected;
}

// ── Wi-Fi setup screen ───────────────────────────────────────────────────────
// active_field: 0=SSID, 1=password, -1=status only (connecting/connected/error)
void display_show_wifi_setup(const char *ssid, const char *pass,
                              int active_field, const char *status) {
    gfx->fillScreen(C_BG);

    // ── Header ──────────────────────────────────────────────────────────────
    gfx->fillRect(0, 0, W, 46, C_HDR);
    // subtle darker top edge for depth
    gfx->fillRect(0, 0, W, 3, 0x1024u);
    gfx->drawFastHLine(0, 45, W, C_ACCENT);
    gfx->drawFastHLine(0, 44, W, C_ACCENT2);

    sans();
    gfx->setTextColor(C_TEXT, C_HDR);
    gfx->setCursor(12, 30);
    gfx->print("Wi-Fi Setup");

    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_DIM, C_HDR);
    gfx->setCursor(188, 18);
    gfx->print("Meshtastic MQTT");

    // ── Input field geometry ─────────────────────────────────────────────────
    const int16_t FX = 12, FW = W - 24, FH = 38;

    // ── SSID field ───────────────────────────────────────────────────────────
    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_DIM, C_BG);
    gfx->setCursor(FX, 58);
    gfx->print("NETWORK");

    bool ssid_active = (active_field == 0);
    gfx->fillRoundRect(FX, 66, FW, FH, 6, C_HDR);
    gfx->drawRoundRect(FX, 66, FW, FH, 6, ssid_active ? C_ACCENT : C_DIM);
    if (ssid_active)
        gfx->drawRoundRect(FX + 1, 67, FW - 2, FH - 2, 5, C_ACCENT2);

    if (ssid && ssid[0]) {
        sans();
        gfx->setTextColor(C_TEXT, C_HDR);
        // show last chars that fit (~36 glyphs ≈ 280 px in FreeSansBold10pt)
        const char *s = ssid;
        if ((int)strlen(ssid) > 36) s = ssid + strlen(ssid) - 36;
        gfx->setCursor(FX + 8, 66 + 27);
        gfx->print(s);

        if (ssid_active) {
            int16_t x1, y1; uint16_t tw, th;
            gfx->getTextBounds(s, FX + 8, 66 + 27, &x1, &y1, &tw, &th);
            int curs_x = FX + 8 + (int)tw + 2;
            if (curs_x < FX + FW - 4)
                gfx->fillRect(curs_x, 66 + 10, 2, 20, C_ACCENT2);
        }
    } else if (ssid_active) {
        // empty field cursor
        gfx->fillRect(FX + 8, 66 + 10, 2, 20, C_ACCENT2);
    }

    // ── Password field ───────────────────────────────────────────────────────
    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_DIM, C_BG);
    gfx->setCursor(FX, 116);
    gfx->print("PASSWORD");

    bool pass_active = (active_field == 1);
    gfx->fillRoundRect(FX, 124, FW, FH, 6, C_HDR);
    gfx->drawRoundRect(FX, 124, FW, FH, 6, pass_active ? C_ACCENT : C_DIM);
    if (pass_active)
        gfx->drawRoundRect(FX + 1, 125, FW - 2, FH - 2, 5, C_ACCENT2);

    if (pass) {
        int pass_len = (int)strlen(pass);
        if (pass_len > 0) {
            // draw filled-circle dots for each password character
            const int DOT_R = 4, DOT_STEP = 14;
            int max_dots = (FW - 20) / DOT_STEP;
            int show_from = (pass_len > max_dots) ? pass_len - max_dots : 0;
            for (int i = show_from; i < pass_len; i++) {
                int dx = FX + 10 + (i - show_from) * DOT_STEP + DOT_R;
                gfx->fillCircle(dx, 124 + FH / 2, DOT_R, C_ACCENT2);
            }
            if (pass_active) {
                int cur_x = FX + 10 + (pass_len - show_from) * DOT_STEP;
                if (cur_x < FX + FW - 6)
                    gfx->fillRect(cur_x, 124 + 10, 2, 20, C_ACCENT2);
            }
        } else if (pass_active) {
            gfx->fillRect(FX + 10, 124 + 10, 2, 20, C_ACCENT2);
        }
    }

    // ── Status line ──────────────────────────────────────────────────────────
    if (status && status[0]) {
        uint16_t sc = C_DIM;
        if (strstr(status, "onnect") && !strstr(status, "ing")) sc = C_OK;
        else if (strstr(status, "ailed") || strstr(status, "rror"))  sc = C_ERR;
        else if (strstr(status, "ing"))                               sc = C_WARN;

        // animated dots for "Connecting..."
        dflt(); gfx->setTextSize(1);
        gfx->setTextColor(sc, C_BG);
        gfx->setCursor(FX, 178);
        gfx->print(status);
    }

    // ── Footer hints ─────────────────────────────────────────────────────────
    gfx->fillRect(0, 192, W, 1, C_HDR);
    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_DIM, C_BG);
    gfx->setCursor(12, 204);
    gfx->print("Tab=switch  Enter=next  Backspace=del");
    gfx->setCursor(12, 220);
    gfx->print("Leave blank + Enter = skip WiFi");
}

void display_show_provisioning_qr(
    const char *dev_eui, const char *app_eui, const char *app_key,
    const char *ble_name, uint32_t pin_code
) {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    char qr_data[72];
    snprintf(qr_data, sizeof(qr_data), "GSF:%s:%s:%s", dev_eui, app_eui, app_key);
    QRCode qrcode;
    uint8_t qr_buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qr_buf, 3, ECC_LOW, qr_data);

    const int QR_PX  = 5;
    const int QR_OFF = (160 - qrcode.size * QR_PX) / 2;
    gfx->fillRect(0, STAT_H, 160, H - STAT_H, C_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++)
        for (uint8_t x = 0; x < qrcode.size; x++) {
            uint16_t c = qrcode_getModule(&qrcode, x, y) ? C_BLACK : C_WHITE;
            gfx->fillRect(QR_OFF + x*QR_PX, STAT_H + 8 + y*QR_PX, QR_PX, QR_PX, c);
        }

    dflt(); gfx->setTextSize(1);
    gfx->setTextColor(C_DIM, C_BG);  gfx->setCursor(168, STAT_H + 12); gfx->print("BLE NAME");
    gfx->setTextColor(C_TEXT, C_BG);
    const char *sfx = strrchr(ble_name, '-');
    gfx->setCursor(168, STAT_H + 22); gfx->print(sfx ? sfx + 1 : ble_name);
    gfx->setTextColor(C_DIM, C_BG);  gfx->setCursor(168, STAT_H + 44); gfx->print("PIN");
    char pin_buf[8]; snprintf(pin_buf, sizeof(pin_buf), "%06lu", (unsigned long)pin_code);
    gfx->setTextSize(2); gfx->setTextColor(C_ACCENT2, C_BG);
    gfx->setCursor(168, STAT_H + 56); gfx->print(pin_buf);
}

static uint8_t read_reg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

void display_show_i2c_scan(uint32_t hold_ms) {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    uint8_t found[16]; int nf = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0 && nf < 16) found[nf++] = a;
    }

    dflt(); gfx->setTextSize(1);
    char hdr[24]; snprintf(hdr, sizeof(hdr), "Found %d device(s):", nf);
    gfx->setTextColor(C_DIM, C_BG); gfx->setCursor(8, STAT_H + 12); gfx->print(hdr);

    int y = STAT_H + 26;
    static const uint8_t PREGS[] = { 0x00, 0x01, 0x02, 0xA3, 0xA7 };
    for (int i = 0; i < nf; i++) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "0x%02X ", found[i]);
        gfx->setTextColor(C_ACCENT2, C_BG); gfx->setCursor(8, y); gfx->print(lbl);
        gfx->setTextColor(C_TEXT, C_BG);
        for (uint8_t r : PREGS) {
            uint8_t v = read_reg(found[i], r);
            char b[8]; snprintf(b, sizeof(b), "%02X:%02X ", r, v); gfx->print(b);
        }
        y += 14;
    }
    delay(hold_ms);
}

// ── P2P pairing screens ───────────────────────────────────────────────────────

void display_show_p2p_mode_select() {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    const int16_t cy = MSG_Y0 + 6;
    tcenter("How is this device connecting?", cy, C_DIM, C_BG);

    // HOST box
    gfx->fillRoundRect(14, cy + 22, 136, 92, 10, C_HDR);
    gfx->drawRoundRect(14, cy + 22, 136, 92, 10, C_ACCENT);
    tcenter("[H]",  14, 136, cy + 30, C_ACCENT2, C_HDR);
    tcenter("HOST", 14, 136, cy + 50, C_TEXT,    C_HDR);
    dflt(); gfx->setTextSize(1); gfx->setTextColor(C_DIM, C_HDR);
    gfx->setCursor(22, cy + 94); gfx->print("generates code");

    // JOIN box
    gfx->fillRoundRect(170, cy + 22, 136, 92, 10, C_HDR);
    gfx->drawRoundRect(170, cy + 22, 136, 92, 10, C_ACCENT);
    tcenter("[J]",  170, 136, cy + 30, C_ACCENT2, C_HDR);
    tcenter("JOIN", 170, 136, cy + 50, C_TEXT,    C_HDR);
    dflt(); gfx->setTextSize(1); gfx->setTextColor(C_DIM, C_HDR);
    gfx->setCursor(178, cy + 94); gfx->print("enters the code");

    dflt(); gfx->setTextSize(1);
    tcenter("Run this on both devices", MSG_Y1 - 18, C_DIM, C_BG, false);
}

void display_show_p2p_host(const char *code) {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    const int16_t cy = MSG_Y0 + 6;
    tcenter("Share this code with the other device:", cy, C_DIM, C_BG);

    gfx->fillRoundRect(40, cy + 22, W - 80, 54, 10, C_HDR);
    gfx->drawRoundRect(40, cy + 22, W - 80, 54, 10, C_ACCENT);

    // Spaced-out code digits
    dflt(); gfx->setTextSize(3); gfx->setTextColor(C_ACCENT2, C_HDR);
    int clen = (int)strlen(code), char_w = 18, gap = 6;
    int total = clen * char_w + (clen - 1) * gap;
    int startx = (W - total) / 2;
    for (int i = 0; i < clen; i++) {
        char ch[2] = { code[i], '\0' };
        gfx->setCursor(startx, cy + 46); gfx->print(ch);
        startx += char_w + gap;
    }

    gfx->drawFastHLine(20, cy + 86, W - 40, C_HDR);
    tcenter("Waiting for joiner...", cy + 96, C_TEXT, C_BG);
    dflt(); gfx->setTextSize(1);
    tcenter("Broadcasting every 2 s", cy + 114, C_DIM, C_BG, false);
}

void display_show_p2p_join(const char *typed, int len) {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    const int16_t cy = MSG_Y0 + 6;
    tcenter("Enter code from other device:", cy, C_DIM, C_BG);

    const int BOX_W = 36, BOX_H = 44, GAP = 6;
    int total_w = 6 * BOX_W + 5 * GAP;
    int start_x = (W - total_w) / 2;
    int box_y   = cy + 22;

    for (int i = 0; i < 6; i++) {
        int bx      = start_x + i * (BOX_W + GAP);
        bool filled = (i < len);
        bool cursor = (i == len);
        uint16_t bg  = filled ? C_BUBBLE_OUT : C_HDR;
        uint16_t bdr = cursor ? C_ACCENT2 : (filled ? C_ACCENT : C_DIM);

        gfx->fillRoundRect(bx, box_y, BOX_W, BOX_H, 6, bg);
        gfx->drawRoundRect(bx, box_y, BOX_W, BOX_H, 6, bdr);

        if (cursor) {
            gfx->fillRect(bx + BOX_W/2 - 2, box_y + 8, 4, BOX_H - 16, C_ACCENT2);
        } else if (filled) {
            dflt(); gfx->setTextSize(2); gfx->setTextColor(C_TEXT, bg);
            char ch[2] = { typed[i], '\0' };
            tcenter(ch, bx, BOX_W, box_y + 12, C_TEXT, bg, false);
        }
    }

    dflt(); gfx->setTextSize(1);
    tcenter("Type 0-9 and A-F",           cy + 78,  C_DIM, C_BG, false);
    tcenter("Listening for host...",       cy + 94,  C_DIM, C_BG, false);
}

void display_show_p2p_paired() {
    gfx->fillScreen(C_BG);
    draw_status_bar();

    const int16_t cy = (MSG_Y0 + MSG_Y1) / 2 - 50;
    gfx->fillRoundRect(20, cy, W - 40, 120, 16, C_HDR);
    gfx->drawRoundRect(20, cy, W - 40, 120, 16, C_ACCENT);
    gfx->drawRoundRect(22, cy + 2, W - 44, 116, 14, C_ACCENT2);

    // Bold checkmark
    const int tx = W / 2, ty = cy + 40;
    for (int t = -2; t <= 2; t++) {
        gfx->drawLine(tx - 20 + t, ty,      tx - 4 + t, ty + 16,  C_OK);
        gfx->drawLine(tx - 4 + t, ty + 16,  tx + 22 + t, ty - 14, C_OK);
    }

    tcenter("PAIRED!", cy + 66, C_ACCENT2, C_HDR);
    dflt(); gfx->setTextSize(1);
    tcenter("Starting secure chat...", cy + 88, C_DIM, C_HDR, false);
}

// ── GluStic Fun: raw draw helpers for the tile map renderer ──────────────────
#ifdef GLUSTIC_FUN
void display_draw_pixels(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *pixels) {
    gfx->draw16bitRGBBitmap(x, y, pixels, w, h);
}
void display_fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color) {
    gfx->fillRect(x, y, w, h, color);
}
void display_fill_circle(int16_t x, int16_t y, uint8_t r, uint16_t color) {
    gfx->fillCircle(x, y, r, color);
}
#endif // GLUSTIC_FUN

// ─────────────────────────────────────────────────────────────────────────────
// Heltec V3 — SSD1306 128×64 OLED via U8g2
// ─────────────────────────────────────────────────────────────────────────────
#else

#include <U8g2lib.h>
#include <Wire.h>
#include <qrcode.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,
    /* reset= */ OLED_RST,
    /* clock= */ OLED_SCL,
    /* data=  */ OLED_SDA
);

static int   s_rssi    = 0;
static char  s_kid[32] = "";

void display_init() {
    u8g2.begin();
    u8g2.setContrast(180);
}

static void header_bar(const char *title) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, title);
    u8g2.drawHLine(0, 9, DISPLAY_WIDTH);
}

void display_show_boot() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13B_tf);
    u8g2.drawStr(14, 28, "Glustick");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(34, 40, "Link");
    u8g2.drawStr(26, 54, FW_VERSION);
    u8g2.sendBuffer();
}

void display_show_provisioning(const char *ble_name) {
    u8g2.clearBuffer();
    header_bar("SETUP");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 22, "Open Glustick app");
    u8g2.drawStr(0, 32, "and tap 'Add device'");
    u8g2.drawStr(0, 44, "BLE:");
    u8g2.drawStr(20, 44, ble_name);
    u8g2.sendBuffer();
}

void display_show_joining() {
    u8g2.clearBuffer();
    header_bar("CONNECTING");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 28, "Joining LoRaWAN...");
    u8g2.drawStr(0, 40, "This can take up to");
    u8g2.drawStr(0, 50, "2 minutes.");
    u8g2.sendBuffer();
}

void display_show_idle(const char *kid_name, int rssi, int battery_pct, uint8_t pending) {
    strncpy(s_kid, kid_name, sizeof(s_kid) - 1);
    s_rssi = rssi;

    u8g2.clearBuffer();
    header_bar("GLUSTICK LINK");

    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(0, 26, kid_name);

    u8g2.setFont(u8g2_font_5x7_tf);
    char rssi_buf[12];
    snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", rssi);
    u8g2.drawStr(DISPLAY_WIDTH - strlen(rssi_buf) * 5, 26, rssi_buf);

    int bar_w = (battery_pct * 30) / 100;
    u8g2.drawFrame(90, 38, 32, 10);
    u8g2.drawBox(91, 39, bar_w, 8);
    char bat_buf[8];
    snprintf(bat_buf, sizeof(bat_buf), "%d%%", battery_pct);
    u8g2.drawStr(90, 58, bat_buf);

    if (pending > 0) {
        char pend_buf[8]; snprintf(pend_buf, sizeof(pend_buf), "!%u", pending);
        u8g2.drawStr(0, 40, pend_buf);
    }
    u8g2.sendBuffer();
}

void display_show_message(const char *from, const char *body) {
    u8g2.clearBuffer();
    header_bar("MESSAGE");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 22, from);
    u8g2.setFont(u8g2_font_5x7_tf);
    char line[22]; int body_len = strlen(body), y = 34;
    for (int i = 0; i < body_len && y <= 60; i += 21) {
        strncpy(line, body + i, 21); line[21] = '\0';
        u8g2.drawStr(0, y, line); y += 10;
    }
    u8g2.sendBuffer();
}

void display_push_message(const char * /*from*/, const char *body, bool /*outgoing*/) {
    display_show_message("", body);
}

void display_scroll(int /*delta*/) {}
void display_touch(int16_t /*x*/, int16_t /*y*/) {}
void display_refresh_status() {}

void display_show_compose(const char * /*draft*/) {}

void display_show_i2c_scan(uint32_t /*hold_ms*/) {}
void display_show_p2p_mode_select() {}
void display_show_p2p_host(const char * /*code*/) {}
void display_show_p2p_join(const char * /*typed*/, int /*len*/) {}
void display_show_p2p_paired() {}

void display_show_repeater(bool listening, uint32_t net_id, int rssi,
                           uint32_t repeats, int battery_pct) {
    u8g2.clearBuffer();
    header_bar("REPEATER");
    u8g2.setFont(u8g2_font_6x10_tf);
    if (listening || net_id == 0) {
        u8g2.drawStr(0, 24, "Listening");
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0, 38, "Hold PRG to forget");
    } else {
        char line[22];
        snprintf(line, sizeof(line), "net %08lX", (unsigned long)net_id);
        u8g2.drawStr(0, 24, line);
        u8g2.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "fwd %lu  %ddBm",
                 (unsigned long)repeats, rssi);
        u8g2.drawStr(0, 38, line);
        snprintf(line, sizeof(line), "bat %d%%", battery_pct);
        u8g2.drawStr(0, 50, line);
        u8g2.drawStr(0, 62, "Hold PRG to forget");
    }
    u8g2.sendBuffer();
}

void display_show_sending(uint8_t pending) {
    u8g2.clearBuffer(); header_bar("SENDING");
    u8g2.setFont(u8g2_font_5x7_tf);
    char buf[32]; snprintf(buf, sizeof(buf), "%u message(s) queued", pending);
    u8g2.drawStr(0, 28, buf); u8g2.drawStr(0, 40, "Transmitting...");
    u8g2.sendBuffer();
}

void display_show_wifi_sync() {
    u8g2.clearBuffer(); header_bar("WIFI SYNC");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 28, "WiFi connected.");
    u8g2.drawStr(0, 40, "Syncing messages...");
    u8g2.sendBuffer();
}

void display_show_error(const char *msg) {
    u8g2.clearBuffer(); u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 10, "ERROR"); u8g2.drawHLine(0, 12, DISPLAY_WIDTH);
    int len = strlen(msg), y = 24; char line[22];
    for (int i = 0; i < len && y <= 60; i += 21) {
        strncpy(line, msg + i, 21); line[21] = '\0';
        u8g2.drawStr(0, y, line); y += 10;
    }
    u8g2.sendBuffer();
}

void display_set_lora_rssi(int rssi) { s_rssi = rssi; }

void display_show_provisioning_qr(
    const char *dev_eui, const char *app_eui, const char *app_key,
    const char *ble_name, uint32_t pin_code
) {
    char qr_data[72];
    snprintf(qr_data, sizeof(qr_data), "GSF:%s:%s:%s", dev_eui, app_eui, app_key);
    QRCode qrcode; uint8_t qrcode_buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcode_buf, 3, ECC_LOW, qr_data);
    u8g2.clearBuffer();
    u8g2.setDrawColor(1); u8g2.drawBox(0, 0, 64, 64); u8g2.setDrawColor(0);
    const int x_off = 3, y_off = 3;
    for (uint8_t y = 0; y < qrcode.size; y++)
        for (uint8_t x = 0; x < qrcode.size; x++)
            if (qrcode_getModule(&qrcode, x, y))
                u8g2.drawBox(x_off + x*2, y_off + y*2, 2, 2);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tf); u8g2.drawStr(66, 10, "SCAN QR");
    u8g2.drawStr(66, 22, "BLE:");
    const char *suffix = strrchr(ble_name, '-');
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(66, 32, suffix ? suffix+1 : ble_name);
    u8g2.setFont(u8g2_font_4x6_tf); u8g2.drawStr(66, 46, "PIN:");
    char pin_buf[8]; snprintf(pin_buf, sizeof(pin_buf), "%06lu", (unsigned long)pin_code);
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(66, 56, pin_buf);
    u8g2.sendBuffer();
}

#endif // BOARD_TDECK
