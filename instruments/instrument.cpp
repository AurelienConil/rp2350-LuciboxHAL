/*
 * instruments/instrument.cpp — Fake instrument, proof-of-life check
 *
 * >>> THIS is the file to edit when deriving your own firmware from this
 *     template. See instrument.hpp and CLAUDE.md. <<<
 */

#include "instrument.hpp"
#include "../hal/audio.h"
#include "../hal/controls.h"
#include "../hal/leds.h"
#include <cmath>
#include <cstdio>

void Instrument::init() {
    /* Precompute normalized sine LUT (amplitude = 1.0 -> int16 range = ±32767) */
    for (int i = 0; i < LUT_SIZE; i++) {
        lut_[i] = (int16_t)(32767.0f * sinf(2.0f * 3.14159265f * i / LUT_SIZE));
    }

    /* Initial frequency: bottom of the sweep range (440 Hz) — LFO phase at 0 */
    phase_inc_ = (uint32_t)(SWEEP_FREQ_LOW_HZ / AUDIO_SAMPLE_RATE * 4294967296.0f);

    /* LFO phase increment per buffer (called once per buffer, not per sample):
     *   sweep_phase_inc = 2π × SWEEP_RATE_HZ × (buffer duration in seconds) */
    sweep_phase_inc_ = 2.0f * 3.14159265f * SWEEP_RATE_HZ
                      * ((float)AUDIO_BUFFER_FRAMES / (float)AUDIO_SAMPLE_RATE);
    sweep_phase_ = 0.0f;

    printf("[Instrument] LUT %d entries, sweep %.0f<->%.0f Hz @ %.1f Hz + ADC passthrough\n",
           LUT_SIZE, SWEEP_FREQ_LOW_HZ, SWEEP_FREQ_HIGH_HZ, SWEEP_RATE_HZ);
}

/* Advances the frequency LFO and recomputes phase_inc_. Called once per buffer. */
void Instrument::update_sweep_() {
    sweep_phase_ += sweep_phase_inc_;
    if (sweep_phase_ > 2.0f * 3.14159265f)
        sweep_phase_ -= 2.0f * 3.14159265f;

    float offset = (float)pitch_offset_hz_.load(std::memory_order_relaxed);
    float freq = SWEEP_CENTER_HZ + offset - SWEEP_AMPLITUDE_HZ * cosf(sweep_phase_);
    if (freq < 20.0f) freq = 20.0f;   /* keep the increment positive */
    phase_inc_ = (uint32_t)(freq / AUDIO_SAMPLE_RATE * 4294967296.0f);
}

/* ── CONTROL side (main loop, ~50 Hz) ─────────────────────────────────────── */

void Instrument::on_button(uint8_t idx, bool pressed) {
    /* Indices 10..25 = pads tactiles (CONTROLS_BTN_TOUCH) : affichés "tactile n"
     * avec n = numéro de pad 0..15 (= CSn = bit n de BUTTON_STAT). */
    if (idx >= CONTROLS_BTN_TOUCH(0))
        printf("[Instrument] tactile %u %s\n", idx - CONTROLS_BTN_TOUCH(0),
               pressed ? "pressed" : "released");
    else
        printf("[Instrument] button %u %s\n", idx, pressed ? "pressed" : "released");

    /* Lit buttons (indices 6..9): toggle their LED on press. */
    if (pressed && idx >= CONTROLS_BTN_LIT(0) &&
        idx < CONTROLS_BTN_LIT(0) + CONTROLS_NUM_BUTTONS_LIT) {
        uint8_t led = idx - CONTROLS_BTN_LIT(0);
        controls_led_set(led, !controls_led_get(led));
    }
}

void Instrument::on_encoder(uint8_t idx, int32_t delta) {
    if (idx != 0) return;
    int32_t off = pitch_offset_hz_.load(std::memory_order_relaxed) + delta * PITCH_STEP_HZ;
    pitch_offset_hz_.store(off, std::memory_order_relaxed);   /* single writer */
    printf("[Instrument] pitch offset %+d Hz\n", (int)off);
}

void Instrument::on_tick(uint32_t now_ms) {
    /* Heartbeat: strip LED 0 blinks at 1 Hz (dim green). */
    bool on = ((now_ms / 500) & 1) != 0;
    leds_set(0, 0, on ? 255 : 0, 0);
}

void Instrument::process(const int16_t *rx, int16_t *tx, uint32_t n_frames) {
    update_sweep_();   /* advance the LFO once per buffer */

    for (uint32_t i = 0; i < n_frames; i++) {
        /* Sweep: LUT index = top 8 bits of the 32-bit phase accumulator */
        uint8_t idx   = (uint8_t)(phase_acc_ >> 24);
        float   sweep = lut_[idx] * SWEEP_GAIN;

        /* Passthrough: mono ADC sample (HAL already extracted the I2S slot) */
        float   pass  = (float)rx[i] * PASSTHROUGH_GAIN;

        /* Mix + int16 clamp */
        float mix = (sweep + pass) * MASTER_GAIN;
        if (mix > 32767.0f)  mix = 32767.0f;
        if (mix < -32768.0f) mix = -32768.0f;
        tx[i] = (int16_t)mix;

        phase_acc_ += phase_inc_;   /* uint32 overflow = natural progression */
    }
}
