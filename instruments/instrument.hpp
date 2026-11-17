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
#include "../hal/tpa6130a2.h"
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
    /* Mixage réglable à l'oreille (control -> audio), en dixièmes de dB :
     *   encodeur 1 = niveau du sweep, encodeur 2 = niveau de la voix (micro).
     * Valeurs de départ ci-dessous ; l'instrument affiche les valeurs finales. */
    static constexpr int32_t MIX_STEP_DB10     = 15;     /* 1.5 dB par cran */
    static constexpr int32_t SWEEP_DB10_INIT   = -365;   /* -36.5 dB ~ x0.015 (réglé à l'oreille) */
    static constexpr int32_t VOICE_DB10_INIT   = 0;      /* 0 dB */
    uint8_t tpa_code_ = TPA_VOLUME;   /* gain TPA courant (contexte control uniquement), réglable au clavier e/d */
    std::atomic<int32_t> sweep_db10_{SWEEP_DB10_INIT};
    std::atomic<int32_t> voice_db10_{VOICE_DB10_INIT};

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

    /* Niveau de sortie global (0..1) appliqué au mix : 0.5 = -6 dB. */
    static constexpr float   MASTER_GAIN      = 1.0f;   /* TEST sinus. Avant : 0.5 */

    /* TEMPORAIRE (debug micro) : false = pas de sweep, seulement le micro. */
    static constexpr bool    SWEEP_ENABLED = false;   /* micro seul */

    /* Audio -> control : niveau micro du dernier buffer (crête, 0..32768) */
    std::atomic<int32_t> mic_peak_{0};
    std::atomic<int32_t> mic_peak_hold_{0};   /* max depuis le dernier log */
    std::atomic<int32_t> out_peak_hold_{0};   /* debug : crête de sortie (avant clamp) */
    std::atomic<uint32_t> clip_hold_{0};      /* debug : échantillons hors plage int16 (saturés par la conversion) */
    std::atomic<int32_t> mic_min_{0}, mic_max_{0}, mic_mean_{0};  /* debug : dernier buffer */

    /* Passe-haut logiciel sur l'entrée micro : Butterworth 4e ordre (24 dB/octave)
     * = 2 biquads en cascade. Coupe le grave (plosives B/P, souffle, dérive DC)
     * que le casque ne restitue pas mais qui monopolise la dynamique.
     * Plus HPF_HZ est haut, plus la voix est fine : 120..250 Hz. */
    static constexpr bool    HPF_ENABLED = true;   /* false = signal ADC strictement brut */
    static constexpr float   HPF_HZ = 100.0f;
    struct Biquad { float b0, b1, b2, a1, a2, x1, x2, y1, y2; };   /* initialisé dans init() */
    Biquad hpf_[2];

    int16_t  lut_[LUT_SIZE];        /* precomputed sine table, amplitude 1.0 */
    uint32_t phase_acc_   = 0;       /* 32-bit phase accumulator (sweep) */
    uint32_t phase_inc_   = 0;       /* current phase increment (sweep) */
    float    sweep_phase_     = 0.0f;  /* frequency LFO phase, radians [0..2π[ */
    float    sweep_phase_inc_ = 0.0f;  /* LFO phase increment per buffer */

    void update_sweep_();            /* advances the LFO and updates phase_inc_ */
};
