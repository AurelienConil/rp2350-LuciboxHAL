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
#include "../hal/tpa6130a2.h"
#include "pico/stdlib.h"
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

    /* Butterworth 4e ordre passe-haut = 2 biquads, Q = 0.5412 et 1.3066,
     * transformée bilinéaire */
    {
        static const float Q[2] = {0.54119610f, 1.30656296f};
        const float K = tanf(3.14159265f * HPF_HZ / (float)AUDIO_SAMPLE_RATE);
        for (int s = 0; s < 2; s++) {
            const float norm = 1.0f / (1.0f + K / Q[s] + K * K);
            hpf_[s].b0 = norm;
            hpf_[s].b1 = -2.0f * norm;
            hpf_[s].b2 = norm;
            hpf_[s].a1 = 2.0f * (K * K - 1.0f) * norm;
            hpf_[s].a2 = (1.0f - K / Q[s] + K * K) * norm;
            hpf_[s].x1 = hpf_[s].x2 = hpf_[s].y1 = hpf_[s].y2 = 0.0f;
        }
    }

    printf("[MIX] clavier console : q/a = sweep, w/s = voix (+/-1.5 dB), e/d = gain TPA casque (+/-1 code)\n");
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
    else if (idx >= CONTROLS_BTN_LIT(0))
        printf("BOUTON_LED %d : GPIO %d %s\n", controls_button_gpio(idx),
               controls_button_gpio(idx), pressed ? "pressed" : "released");
    else
        printf("BOUTON %d : GPIO %d %s\n", controls_button_gpio(idx),
               controls_button_gpio(idx), pressed ? "pressed" : "released");

    /* Lit buttons (indices 6..9): toggle their LED on press. */
    if (pressed && idx >= CONTROLS_BTN_LIT(0) &&
        idx < CONTROLS_BTN_LIT(0) + CONTROLS_NUM_BUTTONS_LIT) {
        uint8_t led = idx - CONTROLS_BTN_LIT(0);
        controls_led_set(led, !controls_led_get(led));
    }
}

void Instrument::on_encoder(uint8_t idx, int32_t delta) {
    if (idx == 1 || idx == 2) {   /* mixage : 1 = sweep, 2 = voix */
        std::atomic<int32_t> &g = (idx == 1) ? sweep_db10_ : voice_db10_;
        const int32_t lo = (idx == 1) ? -600 : -200;   /* -60 dB / -20 dB */
        const int32_t hi = (idx == 1) ?    0 :  300;   /*   0 dB / +30 dB */
        int32_t v = g.load(std::memory_order_relaxed) + delta * MIX_STEP_DB10;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        g.store(v, std::memory_order_relaxed);   /* single writer */
        const int32_t a = sweep_db10_.load(std::memory_order_relaxed);
        const int32_t b = voice_db10_.load(std::memory_order_relaxed);
        printf("[MIX] sweep %+.1f dB (x%.3f) | voix %+.1f dB (x%.3f)\n",
               (double)a / 10.0, (double)powf(10.0f, (float)a / 200.0f),
               (double)b / 10.0, (double)powf(10.0f, (float)b / 200.0f));
        return;
    }
    if (idx != 0) return;
    int32_t off = pitch_offset_hz_.load(std::memory_order_relaxed) + delta * PITCH_STEP_HZ;
    pitch_offset_hz_.store(off, std::memory_order_relaxed);   /* single writer */
    printf("[Instrument] pitch offset %+d Hz\n", (int)off);
}

void Instrument::on_tick(uint32_t now_ms) {
    /* Mixage au clavier (console série USB), sans capteurs branchés :
     *   q / a : sweep  +1.5 dB / -1.5 dB      w / s : voix  +1.5 dB / -1.5 dB */
    for (int c = getchar_timeout_us(0); c != PICO_ERROR_TIMEOUT; c = getchar_timeout_us(0)) {
        switch (c) {
            case 'q': on_encoder(1, +1); break;
            case 'a': on_encoder(1, -1); break;
            case 'w': on_encoder(2, +1); break;
            case 's': on_encoder(2, -1); break;
            case 'e': case 'd': {   /* gain TPA (casque) : e = +1 code, d = -1 code */
                int code = (int)tpa_code_ + (c == 'e' ? +1 : -1);
                if (code < TPA_VOLUME_MIN) code = TPA_VOLUME_MIN;
                if (code > TPA_VOLUME_MAX) code = TPA_VOLUME_MAX;
                tpa_code_ = (uint8_t)code;
                tpa_set_volume(tpa_code_);
                printf("[TPA] volume code 0x%02X = %.1f dB\n", tpa_code_,
                       (double)tpa_volume_to_db(tpa_code_));
                break;
            }
            default: break;
        }
    }

    /* Heartbeat: strip LED 0 blinks at 1 Hz (dim green). */
    bool on = ((now_ms / 500) & 1) != 0;
    leds_set(0, 0, on ? 255 : 0, 0);

    /* Niveau micro : log toutes les 200 ms (crête max depuis le dernier log) */
    static uint32_t last_log_ms = 0;
    if (now_ms - last_log_ms >= 200) {
        last_log_ms = now_ms;
        int32_t pk = mic_peak_hold_.exchange(0, std::memory_order_relaxed);
        int bars = (int)(pk * 30 / 32768);
        char bar[31];
        for (int i = 0; i < 30; i++) bar[i] = i < bars ? '#' : '.';
        bar[30] = 0;
        {   /* mots I2S bruts [L:16][R:16] : voir l'alignement des bits */
            uint32_t w[8];
            audio_debug_rx_words(w);
            printf("[RAW] %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
                   (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2],
                   (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5],
                   (unsigned long)w[6], (unsigned long)w[7]);
        }
        {   /* sortie + timing + calage FIFO : cause des "boosts saturés" ? */
            uint32_t pmax, dtmax; bool txs, rxs;
            audio_debug_timing(&pmax, &dtmax, &txs, &rxs);
            printf("[OUT] peak %5d int16-sat %lu | callback max %lu us, IRQ dt max %lu us | TXSTALL=%d RXSTALL=%d\n",
                   (int)out_peak_hold_.exchange(0, std::memory_order_relaxed),
                   (unsigned long)clip_hold_.exchange(0, std::memory_order_relaxed),
                   (unsigned long)pmax, (unsigned long)dtmax, (int)txs, (int)rxs);
        }
        int32_t pl, pr; uint32_t orr;
        audio_debug_rx_peaks(&pl, &pr, &orr);
        printf("[MIC] peak %5d (%3d%%) |%s| raw L=%d R=%d OR=0x%08lX min=%d max=%d mean=%d\n",
               (int)pk, (int)(pk * 100 / 32768), bar, (int)pl, (int)pr,
               (unsigned long)orr, (int)mic_min_.load(std::memory_order_relaxed),
               (int)mic_max_.load(std::memory_order_relaxed),
               (int)mic_mean_.load(std::memory_order_relaxed));
    }
}

void Instrument::process(const int16_t *rx, int16_t *tx, uint32_t n_frames) {
    update_sweep_();   /* advance the LFO once per buffer */

    int32_t peak = 0, vmin = 32767, vmax = -32768;
    int32_t sum = 0;
    for (uint32_t i = 0; i < n_frames; i++) {
        int32_t v = rx[i];
        int32_t a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        sum += v;
    }
    mic_min_.store(vmin, std::memory_order_relaxed);
    mic_max_.store(vmax, std::memory_order_relaxed);
    mic_mean_.store(sum / (int32_t)n_frames, std::memory_order_relaxed);
    mic_peak_.store(peak, std::memory_order_relaxed);
    if (peak > mic_peak_hold_.load(std::memory_order_relaxed))
        mic_peak_hold_.store(peak, std::memory_order_relaxed);

    /* Gains de mixage, recalculés une fois par buffer depuis les encodeurs 1 et 2 */
    const float sweep_g = powf(10.0f, (float)sweep_db10_.load(std::memory_order_relaxed) / 200.0f);
    const float voice_g = powf(10.0f, (float)voice_db10_.load(std::memory_order_relaxed) / 200.0f);

    float out_pk = 0.0f;
    uint32_t clips = 0;
    for (uint32_t i = 0; i < n_frames; i++) {
        /* Sweep: LUT index = top 8 bits of the 32-bit phase accumulator */
        uint8_t idx   = (uint8_t)(phase_acc_ >> 24);
        float   sweep = SWEEP_ENABLED ? lut_[idx] * sweep_g : 0.0f;

        /* Passthrough: mono ADC sample (HAL already extracted the I2S slot) */
        float   y     = (float)rx[i];
        for (int st = 0; HPF_ENABLED && st < 2; st++) {
            Biquad &f = hpf_[st];
            float o = f.b0 * y + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
            f.x2 = f.x1;  f.x1 = y;
            f.y2 = f.y1;  f.y1 = o;
            y = o;
        }
        float   pass  = y * voice_g;

        /* Mix + int16 clamp */
        float mix = (sweep + pass) * MASTER_GAIN;
        float amix = mix < 0.0f ? -mix : mix;
        if (amix > out_pk) out_pk = amix;

        /* Aucun limiteur ni écrêtage volontaire : signal brut. Seule la conversion
         * en int16 sature, pour éviter un repliement (wrap) si |mix| > 32767. */
        if (amix > 32767.0f) clips++;
        if (mix > 32767.0f)  mix = 32767.0f;
        if (mix < -32768.0f) mix = -32768.0f;
        tx[i] = (int16_t)mix;

        phase_acc_ += phase_inc_;   /* uint32 overflow = natural progression */
    }
    if ((int32_t)out_pk > out_peak_hold_.load(std::memory_order_relaxed))
        out_peak_hold_.store((int32_t)out_pk, std::memory_order_relaxed);
    if (clips) clip_hold_.fetch_add(clips, std::memory_order_relaxed);
}
