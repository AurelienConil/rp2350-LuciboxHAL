/*
 * instruments/instrument.hpp — Fake instrument, proof-of-life check
 *
 * >>> THIS is the file to edit when deriving your own firmware from this
 *     template. Rewrite this class (keep the name `Instrument` and the file
 *     names: main.cpp and CMakeLists.txt then need no change).
 *     See CLAUDE.md for the step-by-step procedure. <<<
 *
 * No musical logic here — just enough to physically verify the whole HAL
 * chain works on all sides:
 *   AUDIO   (process)
 *   - DAC : 440<->470 Hz sweep (0.1 Hz LFO) — listen on headphones/speaker
 *   - ADC : passthrough of the captured signal (mic/line), mixed with the sweep
 *   CONTROL (on_button / on_encoder / on_tick)
 *   - encoder 0 : shifts the sweep pitch by ±5 Hz per detent (control -> audio
 *                 crossing through a std::atomic)
 *   - any button: prints the event; lit buttons toggle their LED
 *   - on_tick   : heartbeat on strip LED 0 (proves the control loop runs)
 *
 * Silence on input -> you only hear the sweep (proves the DAC path).
 * Signal on input (voice, line) -> it adds to the sweep (proves the ADC path).
 * Nothing touched -> exactly the original sweep + passthrough.
 */

#pragma once

#include "audio_instrument.hpp"
#include <atomic>
#include <cstdint>

class Instrument : public AudioInstrument {
public:
    void init() override;
    void process(const int16_t *rx, int16_t *tx, uint32_t n_frames) override;
    void on_button(uint8_t idx, bool pressed) override;
    void on_encoder(uint8_t idx, int32_t delta) override;
    void on_tick(uint32_t now_ms) override;
    const char *name() const override { return "Instrument (selftest)"; }

private:
    /* Control -> audio: pitch offset in Hz, written by on_encoder(), read once
     * per buffer by process(). Atomic 32-bit = lock-free on Cortex-M33. */
    static constexpr int32_t PITCH_STEP_HZ = 5;
    std::atomic<int32_t> pitch_offset_hz_{0};

    static constexpr int     LUT_SIZE = 256;

    /* DAC proof sweep — 440..470 Hz swing at 0.1 Hz */
    static constexpr float   SWEEP_FREQ_LOW_HZ  = 440.0f;
    static constexpr float   SWEEP_FREQ_HIGH_HZ = 470.0f;
    static constexpr float   SWEEP_RATE_HZ      = 0.1f;
    static constexpr float   SWEEP_CENTER_HZ    = (SWEEP_FREQ_LOW_HZ + SWEEP_FREQ_HIGH_HZ) / 2.0f;
    static constexpr float   SWEEP_AMPLITUDE_HZ = (SWEEP_FREQ_HIGH_HZ - SWEEP_FREQ_LOW_HZ) / 2.0f;

    /* Sweep/passthrough mix gains — sum < 1.0 to leave headroom before clamping */
    static constexpr float   SWEEP_GAIN       = 0.5f;
    static constexpr float   PASSTHROUGH_GAIN = 0.5f;
    /* Niveau de sortie global (0..1) appliqué au mix : 0.5 = -6 dB. */
    static constexpr float   MASTER_GAIN      = 0.5f;

    int16_t  lut_[LUT_SIZE];         /* precomputed sine table, amplitude 1.0 */
    uint32_t phase_acc_   = 0;       /* 32-bit phase accumulator (sweep) */
    uint32_t phase_inc_   = 0;       /* current phase increment (sweep) */
    float    sweep_phase_     = 0.0f;  /* frequency LFO phase, radians [0..2π[ */
    float    sweep_phase_inc_ = 0.0f;  /* LFO phase increment per buffer */

    void update_sweep_();            /* advances the LFO and updates phase_inc_ */
};
