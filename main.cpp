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
#include "hal/tpa6130a2.h"
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
    bool tpa_ok = (found & I2C_FOUND_TPA6130A2) && tpa_init();

    /* ── Bring-up summary ─────────────────────────────────────────────── */
    printf("\n=========== BRING-UP SUMMARY ===========\n");
    printf(" I2C bus        : %s\n", bus_ok ? "ok" : "FAILED (line stuck low)");
    printf(" ES8311 (0x18)  : %s\n", codec_ok ? "ok" : "MISSING / init failed");
    printf(" TPA6130A2(0x60): %s\n", tpa_ok ? "ok"
           : (found & I2C_FOUND_TPA6130A2) ? "answers but init failed"
           : "MISSING (check SD pin high)");
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
