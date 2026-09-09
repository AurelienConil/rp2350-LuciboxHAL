/*
 * main.cpp — rp2350-LuciboxHAL
 *
 * Audio firmware for RP2350 + ES8311 — template.
 * Active instrument: SelfTest (DAC sweep + ADC passthrough, proof that the
 * full chain works). To derive a project from this template, replace
 * SelfTest with your own instrument (see CLAUDE.md).
 *
 * Pinout:
 *   GPIO 6/7   : I2C1 SDA/SCL -> ES8311
 *   GPIO 10/11 : I2S BCLK/LRCK (PIO0)
 *   GPIO 12    : DACDAT I2S TX
 *   GPIO 13    : ADCDAT I2S RX
 *   GPIO 14    : MCLK PWM (12.5 MHz)
 *   GPIO 26-29 : internal ADC -> potentiometers
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
#include "hal/es8311.h"
#include "hal/controls.h"
#include "instruments/audio_instrument.hpp"
#include "instruments/selftest.hpp"

static SelfTest g_instrument;

/* ── C callback passed to the HAL — dispatches to the instrument ────────────── */
extern "C" void audio_cb(const int16_t *rx, int16_t *tx, uint32_t n, void *ctx) {
    static_cast<AudioInstrument *>(ctx)->process(rx, tx, n);
}

/* ── I2C pinout ────────────────────────────────────────────────────────────── */
static constexpr int PIN_SDA = 6;
static constexpr int PIN_SCL = 7;

/* ── Entry point ──────────────────────────────────────────────────────────── */
int main() {
    stdio_init_all();
    sleep_ms(1000);  /* wait for the USB CDC link */

    printf("\n==============================================\n");
    printf(" rp2350-LuciboxHAL — instrument: %s\n", g_instrument.name());
    printf(" %d Hz | %d bits | %d frames/buf\n",
           AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH, AUDIO_BUFFER_FRAMES);
    printf("==============================================\n\n");

    /* ── 1. I2C ─────────────────────────────────────────────────────────────
     * Must be initialized BEFORE MCLK (MCLK PWM EMI disturbs I2C transactions).
     * 100 kHz max — do not use 400 kHz while MCLK is active. */
    printf("[INIT 1/7] I2C1 @ %d kHz...\n", ES8311_I2C_SPEED / 1000);
    i2c_init(i2c1, ES8311_I2C_SPEED);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    sleep_ms(10);

    /* ── 2. ES8311 registers ───────────────────────────────────────────── */
    printf("[INIT 2/7] ES8311 (DAC + ADC)...\n");
    if (!es8311_init()) {
        printf("[FATAL] ES8311 not detected. Halting.\n");
        while (true) tight_loop_contents();
    }

    /* ── 3. Controls ──────────────────────────────────────────────────── */
    printf("[INIT 3/7] Controls (internal ADC pots)...\n");
    controls_init();

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
    es8311_power_up();
    audio_hal_start();

    printf("\n[OK] %s active — DMA + PIO free-running, CPU idle.\n\n",
           g_instrument.name());

    /* Main loop: non-real-time control updates */
    while (true) {
        /* MPR121: polled from the main loop (not from the IRQ — I2C is blocking) */
        controls_mpr121_update();
        sleep_ms(10);  /* 100 Hz touch refresh rate */
    }
}
