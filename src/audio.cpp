#if defined(FEATURE_VOICE) && defined(BOARD_TDECK)

#include "audio.h"
#include <Wire.h>
#include <driver/i2s.h>
#include "es7210.h"   // vendored LilyGo driver: lib/es7210

// ── T-Deck audio pins (verified against LILYGO examples/UnitTest/utilities.h) ─
#ifndef BOARD_I2S_WS
#  define BOARD_I2S_WS      5   // speaker amp LRCLK
#endif
#ifndef BOARD_I2S_BCK
#  define BOARD_I2S_BCK     7   // speaker amp BCLK
#endif
#ifndef BOARD_I2S_DOUT
#  define BOARD_I2S_DOUT    6   // speaker amp DIN
#endif
#ifndef BOARD_ES7210_MCLK
#  define BOARD_ES7210_MCLK 48
#endif
#ifndef BOARD_ES7210_SCK
#  define BOARD_ES7210_SCK  47
#endif
#ifndef BOARD_ES7210_LRCK
#  define BOARD_ES7210_LRCK 21
#endif
#ifndef BOARD_ES7210_DIN
#  define BOARD_ES7210_DIN  14
#endif

// I2C for the ES7210 shares the keyboard bus (SDA 18 / SCL 8). keyboard_init()
// already calls Wire.begin() on those pins in the T-Deck build; we guard below
// in case audio_init() runs first.
#ifndef KB_SDA
#  define KB_SDA 18
#endif
#ifndef KB_SCL
#  define KB_SCL 8
#endif

#define SPK_PORT I2S_NUM_0
#define MIC_PORT I2S_NUM_1

static bool s_mic_ok = false;

// Scratch for de-interleaving the 2-slot (TDM ch0|ch1) mic stream down to mono.
static int16_t s_mic_stereo[640];  // 320 mono samples * 2 slots

// ── Speaker: I2S0 MASTER/TX, 8 kHz, 16-bit, mono (left only) ─────────────────
static bool init_speaker() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = AUDIO_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,  // auto-zero on underrun -> silence, not a click
        .fixed_mclk           = 0,
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,  // class-D amp needs no MCLK
        .bck_io_num   = BOARD_I2S_BCK,
        .ws_io_num    = BOARD_I2S_WS,
        .data_out_num = BOARD_I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    if (i2s_driver_install(SPK_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(SPK_PORT, &pins) != ESP_OK)               return false;
    i2s_zero_dma_buffer(SPK_PORT);
    return true;
}

// ── Mic: ES7210 ADC over I2C + I2S1 MASTER/RX, 8 kHz, 16-bit ─────────────────
static bool init_mic() {
    audio_hal_codec_config_t cfg = {
        .adc_input  = AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface  = {
            .mode    = AUDIO_HAL_MODE_SLAVE,       // ES7210 is the I2S slave; ESP clocks it
            .fmt     = AUDIO_HAL_I2S_NORMAL,
            .samples = AUDIO_HAL_08K_SAMPLES,      // 8 kHz — Codec2 native
            .bits    = AUDIO_HAL_BIT_LENGTH_16BITS,
        },
    };

    if (es7210_adc_init(&Wire, &cfg) != ESP_OK) return false;
    es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
    // The T-Deck's physical mic sits on MIC3/MIC4 — give them real gain, leave 1/2 flat.
    es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
                        (es7210_gain_value_t)GAIN_0DB);
    es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
                        (es7210_gain_value_t)GAIN_37_5DB);
    es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

    i2s_config_t i2s = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = AUDIO_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ALL_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0,
        .mclk_multiple        = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan        = I2S_BITS_PER_CHAN_16BIT,
        .chan_mask            = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = BOARD_ES7210_MCLK,
        .bck_io_num   = BOARD_ES7210_SCK,
        .ws_io_num    = BOARD_ES7210_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = BOARD_ES7210_DIN,
    };
    if (i2s_driver_install(MIC_PORT, &i2s, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(MIC_PORT, &pins) != ESP_OK)               return false;
    i2s_zero_dma_buffer(MIC_PORT);
    return true;
}

bool audio_init() {
    // ES7210 lives on the keyboard I2C bus; make sure it's up even if audio
    // initialises before the keyboard.
    Wire.begin(KB_SDA, KB_SCL);

    bool spk = init_speaker();
    s_mic_ok = init_mic();
    return spk;  // speaker is the hard requirement; mic can be bench-tuned later
}

bool audio_mic_ok() { return s_mic_ok; }

size_t audio_mic_read(int16_t *buf, size_t samples) {
    if (!s_mic_ok) return 0;
    size_t got = 0;
    while (got < samples) {
        size_t want = samples - got;
        if (want > 320) want = 320;             // bounded by s_mic_stereo/2
        size_t bytes_read = 0;
        // Read `want` stereo frames (2 slots each) -> want*2 int16.
        esp_err_t rc = i2s_read(MIC_PORT, (char *)s_mic_stereo,
                                want * 2 * sizeof(int16_t), &bytes_read,
                                pdMS_TO_TICKS(60));
        if (rc != ESP_OK || bytes_read == 0) break;
        size_t frames = bytes_read / (2 * sizeof(int16_t));
        // Keep the left slot of each frame -> mono.
        for (size_t i = 0; i < frames; i++) buf[got + i] = s_mic_stereo[i * 2];
        got += frames;
    }
    return got;
}

void audio_speaker_write(const int16_t *buf, size_t samples) {
    size_t written = 0;
    i2s_write(SPK_PORT, buf, samples * sizeof(int16_t), &written, portMAX_DELAY);
}

void audio_speaker_silence(size_t samples) {
    static const int16_t zeros[128] = {0};
    while (samples) {
        size_t n = samples > 128 ? 128 : samples;
        audio_speaker_write(zeros, n);
        samples -= n;
    }
}

#endif // FEATURE_VOICE && BOARD_TDECK
