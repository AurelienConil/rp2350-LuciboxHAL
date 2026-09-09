/*
 * instruments/selftest.hpp — Fake instrument, proof-of-life check
 *
 * >>> THIS is the file to edit when deriving your own firmware from this
 *     template. Replace SelfTest with your own instrument (new class
 *     inheriting from AudioInstrument), update main.cpp and CMakeLists.txt
 *     accordingly. See CLAUDE.md for the step-by-step procedure. <<<
 *
 * No musical logic here — just enough to physically verify the whole HAL
 * chain works on both sides:
 *   - DAC : 440<->470 Hz sweep (0.1 Hz LFO) — listen on headphones/speaker
 *   - ADC : passthrough of the captured signal (mic/line), mixed with the sweep
 *
 * Silence on input -> you only hear the sweep (proves the DAC path).
 * Signal on input (voice, line) -> it adds to the sweep (proves the ADC path).
 */

#pragma once

#include "audio_instrument.hpp"
#include <cstdint>

class SelfTest : public AudioInstrument {
public:
    void init() override;
    void process(const int16_t *rx, int16_t *tx, uint32_t n_frames) override;
    const char *name() const override { return "SelfTest"; }

private:
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

    int16_t  lut_[LUT_SIZE];         /* precomputed sine table, amplitude 1.0 */
    uint32_t phase_acc_   = 0;       /* 32-bit phase accumulator (sweep) */
    uint32_t phase_inc_   = 0;       /* current phase increment (sweep) */
    float    sweep_phase_     = 0.0f;  /* frequency LFO phase, radians [0..2π[ */
    float    sweep_phase_inc_ = 0.0f;  /* LFO phase increment per buffer */

    void update_sweep_();            /* advances the LFO and updates phase_inc_ */
};
