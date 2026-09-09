/*
 * instruments/audio_instrument.hpp — Abstract base class for all instruments
 *
 * Each instrument inherits from AudioInstrument and implements:
 *   - init()    : resource initialization (LUT, buffers, etc.)
 *   - process() : per-buffer audio processing (called from DMA IRQ via the HAL)
 *   - name()    : short name for debugging
 *
 * process() constraints (inherited from the IRQ context):
 *   - Duration < 5 ms (256-sample buffer size @ 48 kHz)
 *   - No dynamic memory allocation
 *   - No printf(), sleep(), or blocking mutex
 *   - Reading controls (pots, MPR121): OK read-only from the IRQ
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
     * process() — per-buffer audio processing.
     * Called from the DMA IRQ at the end of every buffer (~5.3 ms @ 48 kHz, 256 frames).
     *
     * rx       : incoming mono ADC samples × n_frames (read-only)
     * tx       : outgoing mono DAC samples to fill × n_frames
     * n_frames : always AUDIO_BUFFER_FRAMES (256)
     */
    virtual void process(const int16_t *rx, int16_t *tx, uint32_t n_frames) = 0;

    /*
     * name() — short instrument name (for logs).
     */
    virtual const char *name() const = 0;
};
