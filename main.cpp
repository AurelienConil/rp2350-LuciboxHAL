/*
 * main.cpp — rp2350-LuciboxHAL
 *
 * Audio firmware for RP2350 + ES8311 — template.
 * Active instrument: Instrument (DAC sweep + ADC passthrough, proof that the
 * full chain works). To derive a project from this template, replace
 * Instrument with your own instrument (see CLAUDE.md).
 *
 * Pinout:
 *   GPIO 16/17 : I2C0 SDA/SCL -> ES8311, TPA6130A2, CY8CMBR3116 (hal/i2c_bus.h)
 *   GPIO 6     : CY8CMBR3116 HI (touch IRQ, active low)
 *   GPIO 10/11 : I2S BCLK/LRCK (PIO0)
 *   GPIO 9     : DACDAT I2S TX (-> ES8311 DSDIN)
 *   GPIO 13    : ADCDAT I2S RX
 *   GPIO 14    : MCLK PWM (12.5 MHz)
 *   GPIO 18-25 : 4 quadrature encoders (A,B pairs)
 *   GPIO 26-31 : 6 simple buttons (pull-up, active-low)
 *   GPIO 32-39 : 4 lit buttons (switch 32/34/36/38, LED 33/35/37/39)
 *   GPIO 1     : WS2812 data (16 LEDs, PIO1)
 *   GPIO 40-47 : internal ADC -> 8 potentiometers (RP2350B)
 *   (full details: hal/controls.h)
 *
 * Init order (hardware constraints):
 *   1. I2C   : before MCLK (PWM EMI disturbs I2C)
 *   2. ES8311 registers
 *   3. Controls (ADC pots)
 *   4. HAL audio init (PIO + DMA, not started)
 *   5. Instrument init
 *   6. MCLK
 *   7. ES8311 power-up (CSM_ON — requires stable MCLK)
 *   8. audio_hal_start() — enables DMA + PIO
 */

#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "hal/audio.h"
#include "hal/i2c_bus.h"
#include "hal/es8311.h"
#include "hal/controls.h"
#include "hal/leds.h"
#include "hal/touch.h"
#include "instruments/audio_instrument.hpp"
#include "instruments/instrument.hpp"   

static Instrument g_instrument;

/* ── Control-side dispatch: controls -> instrument hooks (main loop only) ──────
 * This is the single reader of button edges and encoder deltas. */
static void dispatch_controls(AudioInstrument &inst) {
    for (uint8_t i = 0; i < CONTROLS_NUM_BUTTONS; i++) {
        while (controls_button_pressed(i))  inst.on_button(i, true);
        while (controls_button_released(i)) inst.on_button(i, false);
    }
    for (uint8_t i = 0; i < CONTROLS_NUM_ENCODERS; i++) {
        int32_t d = controls_encoder_delta(i);
        if (d != 0) inst.on_encoder(i, d);
    }
    inst.on_tick(to_ms_since_boot(get_absolute_time()));
}

/* ── C callback passed to the HAL — dispatches to the instrument ────────────── */
extern "C" void audio_cb(const int16_t *rx, int16_t *tx, uint32_t n, void *ctx) {
    static_cast<AudioInstrument *>(ctx)->process(rx, tx, n);
}

/* ── TPA6130A2 : init minimale + volume (pas de vrai driver) ──────────────────
 * Datasheet SLOS488F, Table 3-5 :
 *   REG1 : HP_EN_L(7) HP_EN_R(6) MODE[1:0](5:4) THERMAL(1) SWS(0). Reset 0x00 =
 *          amplis désactivés -> il faut mettre bits 7 et 6 à 1. Bits 3:2 réservés.
 *   REG2 : MUTE_L(7) MUTE_R(6) VOLUME[5:0]. Reset 0xC0 = muté.
 *   REG4 : version (attendu 0x02).
 * Le volume suit une courbe audio NON linéaire (Table 2) : voir TPA_VOLUME_DB. */

/* Gain nominal (dB) pour VOLUME[5:0] = 0..63 (datasheet, Table 2). */
static const float TPA_VOLUME_DB[64] = {
    -59.5f,-53.5f,-50.0f,-47.5f,-45.5f,-43.9f,-41.4f,-39.5f,
    -36.5f,-35.3f,-33.3f,-31.7f,-30.4f,-28.6f,-27.1f,-26.3f,
    -24.7f,-23.7f,-22.5f,-21.7f,-20.5f,-19.6f,-18.8f,-17.8f,
    -17.0f,-16.2f,-15.2f,-14.5f,-13.7f,-13.0f,-12.3f,-11.6f,
    -10.9f,-10.3f, -9.7f, -9.0f, -8.5f, -7.8f, -7.2f, -6.7f,
     -6.1f, -5.6f, -5.1f, -4.5f, -4.1f, -3.5f, -3.1f, -2.6f,
     -2.1f, -1.7f, -1.2f, -0.8f, -0.3f,  0.1f,  0.5f,  0.9f,
      1.4f,  1.7f,  2.1f,  2.5f,  2.9f,  3.3f,  3.6f,  4.0f };

/* Volume casque, code 0..63. Quelques repères : 0x05 = -43.9 dB, 0x08 = -36.5 dB,
 * 0x0C = -30.4 dB, 0x10 = -24.7 dB, 0x18 = -17.0 dB, 0x30 = -2.1 dB (très fort). */
#ifndef TPA_VOLUME
#define TPA_VOLUME 0x05
#endif

static bool tpa_read(uint8_t reg, uint8_t *v) {
    return i2c_write_blocking(LUCIBOX_I2C, I2C_ADDR_TPA6130A2, &reg, 1, true) == 1 &&
           i2c_read_blocking(LUCIBOX_I2C, I2C_ADDR_TPA6130A2, v, 1, false) == 1;
}
static void tpa_diag_and_unmute(void) {
    uint8_t r[5] = {0};
    for (uint8_t i = 1; i <= 4; i++) {
        if (!tpa_read(i, &r[i])) { printf("[TPA] read REG0x%02X failed\n", i); return; }
    }
    printf("[TPA] before: REG1=0x%02X REG2=0x%02X REG3=0x%02X REG4(version)=0x%02X\n",
           r[1], r[2], r[3], r[4]);
    const uint8_t vol = (uint8_t)(TPA_VOLUME & 0x3F);
    uint8_t w1[2] = {0x01, 0xC0};   /* HP_EN_L + HP_EN_R, stéréo (MODE=00), SWS=0 */
    uint8_t w2[2] = {0x02, vol};    /* L/R non mutés, volume */
    i2c_write_blocking(LUCIBOX_I2C, I2C_ADDR_TPA6130A2, w1, 2, false);
    i2c_write_blocking(LUCIBOX_I2C, I2C_ADDR_TPA6130A2, w2, 2, false);
    tpa_read(1, &r[1]); tpa_read(2, &r[2]);
    printf("[TPA] after : REG1=0x%02X REG2=0x%02X (volume code 0x%02X = %.1f dB)\n",
           r[1], r[2], vol, (double)TPA_VOLUME_DB[vol]);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */
int main() {
    stdio_init_all();
    /* Attendre l'ouverture du terminal série (max 10 s) pour ne pas perdre
     * le log de boot ; au-delà, on démarre quand même (usage sans PC). */
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(300);  /* laisser le terminal finir de s'ouvrir */

    printf("\n==============================================\n");
    printf(" rp2350-LuciboxHAL — instrument: %s\n", g_instrument.name());
    printf(" %d Hz | %d bits | %d frames/buf\n",
           AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH, AUDIO_BUFFER_FRAMES);
    printf("==============================================\n\n");

    /* ── 1. I2C ─────────────────────────────────────────────────────────────
     * Must be initialized BEFORE MCLK (MCLK PWM EMI disturbs I2C transactions).
     * 100 kHz max — do not use 400 kHz while MCLK is active. */
    printf("[INIT 1/7] I2C bus + device scan...\n");
    bool     bus_ok = i2c_bus_init();
    uint32_t found  = bus_ok ? i2c_bus_scan() : 0;

    /* ── 2. ES8311 registers ───────────────────────────────────────────── */
    printf("[INIT 2/7] ES8311 (DAC + ADC)...\n");
    bool codec_ok = bus_ok && es8311_init();
    if (!codec_ok) {
        /* Bring-up friendly: keep going so the other components (controls,
         * touch, LEDs) can still be tested. No sound without the codec. */
        printf("[ES8311] NOT AVAILABLE — continuing WITHOUT audio.\n");
    }

    /* ── 3. Controls ──────────────────────────────────────────────────── */
    printf("[INIT 3/7] Controls (internal ADC pots)...\n");
    controls_init();
    leds_init();   /* WS2812 strip, GPIO 1, PIO1 */

    /* ── 4. HAL audio (PIO + DMA) ─────────────────────────────────────── */
    printf("[INIT 4/7] Audio HAL (PIO I2S duplex + DMA double-buffer)...\n");
    audio_hal_init(audio_cb, &g_instrument);

    /* ── 5. Instrument ────────────────────────────────────────────────── */
    printf("[INIT 5/7] Instrument: %s...\n", g_instrument.name());
    g_instrument.init();

    /* ── 6. MCLK ──────────────────────────────────────────────────────── */
    printf("[INIT 6/7] MCLK PWM...\n");
    mclk_init();
    sleep_ms(5);   /* PWM stabilization before triggering CSM */

    /* ── 7. ES8311 power-up + start audio ────────────────────────────── */
    printf("[INIT 7/7] ES8311 power-up (CSM_ON) + starting stream...\n");
    if (codec_ok) es8311_power_up();
    audio_hal_start();
    if (found & I2C_FOUND_TPA6130A2) tpa_diag_and_unmute();

    /* ── Bring-up summary ─────────────────────────────────────────────── */
    printf("\n=========== BRING-UP SUMMARY ===========\n");
    printf(" I2C bus        : %s\n", bus_ok ? "ok" : "FAILED (line stuck low)");
    printf(" ES8311 (0x18)  : %s\n", codec_ok ? "ok" : "MISSING / init failed");
    printf(" TPA6130A2(0x60): %s\n", (found & I2C_FOUND_TPA6130A2)
           ? "answers on the bus (no driver yet)" : "MISSING (check SD pin high)");
    printf(" Touch (0x37)   : %s\n", touch_present() ? "ok (16 pads)"
           : "MISSING / init failed");
    printf("=======================================\n");

    printf("\n[OK] %s active — DMA + PIO free-running, CPU idle.\n\n",
           g_instrument.name());

    /* Main loop: control rate (~50 Hz), never from the audio IRQ.
     * scan sensors -> deliver events to the instrument hooks -> refresh LEDs. */
    while (true) {
        controls_update();
        dispatch_controls(g_instrument);
        leds_show();   /* push the framebuffer written by the instrument */
        sleep_ms(CONTROLS_UPDATE_PERIOD_MS);
    }
}
