/*
 * instruments/audio_instrument.hpp — Abstract base class for all instruments
 *
 * An instrument lives in TWO execution contexts, with very different rules:
 *
 *   AUDIO   (DMA IRQ, ~5.3 ms/buffer)   : process()
 *   CONTROL (main loop, ~50 Hz, no IRQ) : on_button(), on_encoder(), on_tick()
 *
 *   controls ──► main loop ──► on_button / on_encoder / on_tick   (UI, LEDs,
 *                                   │                              sequencer state)
 *                                   ▼
 *                      shared state (atomics / lock-free queue)
 *                                   │
 *   DMA IRQ ──────────────────────► process()                    (synthesis)
 *
 * The final user only rewrites ONE class, `Instrument`, inheriting from
 * AudioInstrument (instruments/instrument.hpp/.cpp — one instrument per
 * firmware, fixed names). main.cpp, CMakeLists.txt and hal/ never change.
 *
 * ── AUDIO side: process() ───────────────────────────────────────────────────
 *   - Duration < 5 ms (256-sample buffer size @ 48 kHz)
 *   - No dynamic memory allocation
 *   - No printf(), sleep(), or blocking mutex
 *   - Controls (hal/controls.h): only the cached level getters —
 *       controls_pot_get(ch)            0..1 smoothed, ch 0..7
 *       controls_button_get(i)          level
 *     Never controls_pot_read_*(). Do NOT read edges/deltas here
 *     (controls_button_pressed/_released, controls_encoder_delta): main()
 *     consumes them and delivers them through the hooks below.
 *   - Anything produced by the hooks and consumed by process() must cross the
 *     context boundary safely: std::atomic<T> (lock-free types, relaxed is
 *     enough for independent parameters) or a single-producer/single-consumer
 *     queue. No mutex.
 *
 * ── CONTROL side: on_button / on_encoder / on_tick ──────────────────────────
 *   Called from main(), after controls_update(), never from an IRQ.
 *   - May be slow-ish (keep each call well under CONTROLS_UPDATE_PERIOD_MS, 20 ms)
 *   - May printf(), write LEDs (leds_set, controls_led_set); main() calls
 *     leds_show() right after the hooks
 *   - Must not call the blocking audio-side APIs, nor assume any timing
 *     tighter than the control period: the musical clock (steps, tempo)
 *     belongs in process() (sample counter), not in on_tick().
 *   All three have an empty default: override only what you need.
 *
 * int16_t[n_frames] buffer format — MONO:
 *   buf[i] = one sample. The ES8311 is a mono codec; the HAL already
 *   extracts/duplicates the I2S L/R slots (see hal/audio.h,
 *   AUDIO_RX_SLOT/AUDIO_TX_SLOT) before/after calling process(). Instruments
 *   never see L/R — only a single mono stream in, one out.
 */

#pragma once

#include <cstdint>

class AudioInstrument {
public:
    virtual ~AudioInstrument() = default;

    /*
     * init() — instrument initialization.
     * Called once from main(), before audio_hal_start().
     * Allowed: printf(), static allocation, slow computations.
     */
    virtual void init() = 0;

    /*
     * process() — per-buffer audio processing. AUDIO context (DMA IRQ).
     * Called at the end of every buffer (~5.3 ms @ 48 kHz, 256 frames).
     *
     * rx       : incoming mono ADC samples × n_frames (read-only)
     * tx       : outgoing mono DAC samples to fill × n_frames
     * n_frames : always AUDIO_BUFFER_FRAMES (256)
     */
    virtual void process(const int16_t *rx, int16_t *tx, uint32_t n_frames) = 0;

    /*
     * on_button() — CONTROL context. A debounced edge, once per event.
     * idx     : 0..CONTROLS_NUM_BUTTONS-1 (CONTROLS_BTN_SIMPLE(n) / CONTROLS_BTN_LIT(n))
     * pressed : true = press, false = release
     * Within one control tick, all presses are delivered before all releases
     * (the relative order of a press and a release of the same tick is lost).
     */
    virtual void on_button(uint8_t idx, bool pressed) { (void)idx; (void)pressed; }

    /*
     * on_encoder() — CONTROL context. Called only when the encoder moved.
     * idx   : 0..CONTROLS_NUM_ENCODERS-1
     * delta : detents since the previous call, signed (clockwise = positive)
     */
    virtual void on_encoder(uint8_t idx, int32_t delta) { (void)idx; (void)delta; }

    /*
     * on_tick() — CONTROL context. Once per control tick (~50 Hz), after the
     * button/encoder hooks. For UI animation (LEDs), pot reading, housekeeping.
     * now_ms : milliseconds since boot.
     */
    virtual void on_tick(uint32_t now_ms) { (void)now_ms; }

    /*
     * name() — short instrument name (for logs).
     */
    virtual const char *name() const = 0;
};
