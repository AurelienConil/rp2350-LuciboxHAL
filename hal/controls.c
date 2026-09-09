/*
 * hal/controls.c — Hardware controls: potentiometers + MPR121 (stub)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/platform.h"   /* __not_in_flash_func() */
#include "hardware/adc.h"
#include "controls.h"

/* ── Potentiometers ───────────────────────────────────────────────────────── */

/* Smoothed cache, updated once per audio block by controls_update() (called
 * from hal/audio.c's dma_irq_handler, before the instrument runs). This is
 * what instruments must read via controls_pot_get() — never the raw ADC. */
static float pot_smoothed[CONTROLS_NUM_POTS];

/* One-pole smoothing coefficient: y += COEFF * (x - y).
 * 0.3 -> time constant ~= 3 blocks ~= 16 ms at the default 256 frames/48 kHz
 * block rate (AUDIO_BUFFER_FRAMES/AUDIO_SAMPLE_RATE). Enough to kill ADC/mux
 * jitter (a few LSB) without making a pot feel sluggish. If AUDIO_BUFFER_FRAMES
 * changes significantly, re-tune this constant. */
#define CONTROLS_POT_SMOOTH_COEFF 0.3f

void controls_init(void) {
    adc_init();
    /* GPIO 26..29 = ADC channels 0..3 */
    for (int ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        adc_gpio_init(26 + ch);
    }
    /* Prime the cache so controls_pot_get() never returns a stale 0.0f
     * before the first controls_update() runs. */
    for (int ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        pot_smoothed[ch] = controls_pot_read_norm((uint8_t)ch);
    }
    printf("[CONTROLS] Internal ADC initialized (%d channels, GPIO 26..%d)\n",
           CONTROLS_NUM_POTS, 26 + CONTROLS_NUM_POTS - 1);
}

uint16_t __not_in_flash_func(controls_pot_read_raw)(uint8_t ch) {
    if (ch >= CONTROLS_NUM_POTS) return 0;
    adc_select_input(ch);
    return adc_read();  /* 12 bits: 0..4095 */
}

float __not_in_flash_func(controls_pot_read_norm)(uint8_t ch) {
    return controls_pot_read_raw(ch) / 4095.0f;
}

void __not_in_flash_func(controls_update)(void) {
    for (uint8_t ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        float raw = controls_pot_read_norm(ch);
        pot_smoothed[ch] += CONTROLS_POT_SMOOTH_COEFF * (raw - pot_smoothed[ch]);
    }
}

float controls_pot_get(uint8_t ch) {
    if (ch >= CONTROLS_NUM_POTS) return 0.0f;
    return pot_smoothed[ch];
}

/* ── MPR121 — STUB ────────────────────────────────────────────────────────── */

static volatile uint16_t mpr121_state = 0;  /* 12-bit mask, atomic to read */

bool controls_mpr121_init(void) {
    /* TODO: implement MPR121 initialization over I2C1
     *   - Check presence (read WHO_AM_I or electrode baseline register)
     *   - Configure touch/release thresholds
     *   - Enable run mode (ELE_CFG register)
     * Return false if the chip is not detected.
     */
    printf("[MPR121] Stub — not implemented (addr 0x5A on I2C1)\n");
    return false;
}

void controls_mpr121_update(void) {
    /* TODO: read the ELE0_ELE7 (0x00) and ELE8_ELEPROX (0x01) registers
     *   via i2c_read on i2c1 (addr 0x5A).
     *   Update mpr121_state atomically.
     *   Call from the main loop (not from the IRQ — I2C is blocking).
     */
    (void)mpr121_state;  /* avoid unused warning */
}

uint16_t controls_mpr121_touched(void) {
    return mpr121_state;  /* atomic uint16_t read (aligned, no tearing) */
}
