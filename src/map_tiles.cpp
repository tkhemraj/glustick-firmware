#ifdef GLUSTIC_FUN
#ifdef BOARD_TDECK

#include "map_tiles.h"
#include "display.h"
#include <TJpg_Decoder.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>

static bool    s_sd_ok = false;
static uint8_t s_zoom  = MAP_DEFAULT_ZOOM;

// ── TJpg_Decoder pixel callback ───────────────────────────────────────────────
// Called for each decoded block; forwards straight to the display.
static bool tile_pixel_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    display_draw_pixels(x, y, w, h, bmp);
    return true;
}

// ── Coordinate math ───────────────────────────────────────────────────────────
// Converts lat/lon to OSM tile address + fractional pixel offset within that tile.
// frac_x / frac_y are in [0, 1) and represent the sub-tile position.
static void latlon_to_tile(double lat, double lon, uint8_t zoom,
                            int32_t *tx, int32_t *ty,
                            double *frac_x, double *frac_y) {
    double n     = (double)(1L << zoom);
    double fx    = (lon + 180.0) / 360.0 * n;
    double lat_r = lat * M_PI / 180.0;
    double fy    = (1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0 * n;
    *tx = (int32_t)floor(fx);
    *ty = (int32_t)floor(fy);
    if (frac_x) *frac_x = fx - floor(fx);
    if (frac_y) *frac_y = fy - floor(fy);
}

// ── Tile renderer ─────────────────────────────────────────────────────────────
static void draw_tile(int32_t tx, int32_t ty, int16_t screen_x, int16_t screen_y) {
    char path[56];
    snprintf(path, sizeof(path), "/tiles/%u/%ld/%ld.jpg",
             (unsigned)s_zoom, (long)tx, (long)ty);

    if (!SD.exists(path)) {
        // Checkerboard placeholder so missing tiles are obvious
        display_fill_rect(screen_x, screen_y, 256, 256, 0x4228);
        return;
    }
    TJpgDec.drawSdJpg(screen_x, screen_y, path);
}

// ── Position dot ─────────────────────────────────────────────────────────────
static void draw_dot(double lat, double lon,
                     int32_t ctile_x, int32_t ctile_y,
                     int16_t tile_sx, int16_t tile_sy,
                     uint16_t ring_color) {
    int32_t dtx, dty;
    double  dfx, dfy;
    latlon_to_tile(lat, lon, s_zoom, &dtx, &dty, &dfx, &dfy);

    int16_t sx = tile_sx + (int16_t)((dtx - ctile_x) * 256 + dfx * 256);
    int16_t sy = tile_sy + (int16_t)((dty - ctile_y) * 256 + dfy * 256);

    if (sx >= 5 && sx < DISPLAY_WIDTH - 5 && sy >= 5 && sy < DISPLAY_HEIGHT - 5) {
        display_fill_circle(sx, sy, 6, ring_color);
        display_fill_circle(sx, sy, 3, 0xFFFF);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
bool map_tiles_init() {
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);  // flip to little-endian for the ESP32 display bus
    TJpgDec.setCallback(tile_pixel_cb);

    if (!SD.begin(TDECK_SD_CS)) {
        return false;
    }
    s_sd_ok = true;
    return true;
}

void map_tiles_set_zoom(uint8_t zoom) {
    s_zoom = zoom;
}

void map_tiles_draw(GpsCoord own, GpsCoord peer) {
    if (!s_sd_ok) {
        display_show_error("No SD card.\nCopy tiles/ folder\nand reboot.");
        return;
    }

    // Fall back to a fixed coord if no GPS fix yet
    double centre_lat = own.valid ? own.lat : 0.0;
    double centre_lon = own.valid ? own.lon : 0.0;

    if (!own.valid) {
        display_show_error("Waiting for\nGPS fix...");
        return;
    }

    // Tile + sub-tile pixel for the centre point
    int32_t ctile_x, ctile_y;
    double  cfrac_x, cfrac_y;
    latlon_to_tile(centre_lat, centre_lon, s_zoom,
                   &ctile_x, &ctile_y, &cfrac_x, &cfrac_y);

    // Screen coordinate of the top-left corner of the centre tile
    int16_t tile_sx = DISPLAY_WIDTH  / 2 - (int16_t)(cfrac_x * 256);
    int16_t tile_sy = DISPLAY_HEIGHT / 2 - (int16_t)(cfrac_y * 256);

    // Tile index range needed to fill 320×240
    int32_t tx0 = ctile_x + (int32_t)floor((0.0 - tile_sx) / 256.0);
    int32_t tx1 = ctile_x + (int32_t)floor(((double)(DISPLAY_WIDTH  - 1) - tile_sx) / 256.0);
    int32_t ty0 = ctile_y + (int32_t)floor((0.0 - tile_sy) / 256.0);
    int32_t ty1 = ctile_y + (int32_t)floor(((double)(DISPLAY_HEIGHT - 1) - tile_sy) / 256.0);

    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            int16_t sx = tile_sx + (int16_t)((tx - ctile_x) * 256);
            int16_t sy = tile_sy + (int16_t)((ty - ctile_y) * 256);
            draw_tile(tx, ty, sx, sy);
        }
    }

    // Blue dot = you, red dot = peer's last known position
    draw_dot(own.lat, own.lon, ctile_x, ctile_y, tile_sx, tile_sy, 0x001F);
    if (peer.valid) {
        draw_dot(peer.lat, peer.lon, ctile_x, ctile_y, tile_sx, tile_sy, 0xF800);
    }
}

#endif // BOARD_TDECK
#endif // GLUSTIC_FUN
