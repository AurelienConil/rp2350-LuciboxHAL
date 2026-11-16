/*
 * hal/audio.h — Audio engine HAL: duplex I2S PIO + DMA double-buffer
 *
 * Public interface for the audio system.
 * The caller provides an `audio_process_fn` callback that receives:
 *   - an ADC input buffer (rx): data from the ES8311 ADC -> RAM
 *   - a DAC output buffer (tx): data from RAM -> ES8311 DAC
 * The callback is called from the DMA IRQ at the end of every buffer.
 *
 * Fixed parameters:
 *   Sample rate : AUDIO_SAMPLE_RATE (48000 Hz)
 *   Bit depth   : AUDIO_BIT_DEPTH   (16 bits/channel)
 *   Wire format : I2S Philips, 2 slots/frame (fixed by the ES8311 and
 *                 i2s_duplex.pio — see CLAUDE.md "Pièges connus", do not change)
 *   Buffer size : AUDIO_BUFFER_FRAMES frames per callback
 *
 * Mono codec, stereo wire:
 *   The ES8311 is a MONO codec (one DAC output, one ADC input), but I2S
 *   Philips always carries 2×16-bit slots per frame — that is the protocol,
 *   not a channel-count choice. The HAL absorbs this entirely: instruments
 *   only ever see MONO int16_t buffers via audio_process_fn. Internally, the
 *   HAL extracts the input slot selected by AUDIO_RX_SLOT and writes the
 *   output to the slot(s) selected by AUDIO_TX_SLOT (below) — resolved at
 *   compile time, zero runtime cost. Instruments never touch L/R.
 *
 * Buffer format (int16_t[AUDIO_BUFFER_FRAMES], mono):
 *   buf[i] = one sample, already extracted/duplicated by the HAL.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Audio parameters (compile-time configurable) ────────────────────────── */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE   48000
#endif

#ifndef AUDIO_BIT_DEPTH
#define AUDIO_BIT_DEPTH     16
#endif

#ifndef AUDIO_BUFFER_FRAMES
#define AUDIO_BUFFER_FRAMES 256   /* 256 / 48000 ≈ 5.3 ms per half-buffer */
#endif

/* ── Mono/stereo slot selection (compile-time) ───────────────────────────────
 * The ES8311 is mono, but the I2S wire format always carries 2 slots/frame.
 * These defines pick which slot(s) carry the mono signal, resolved entirely
 * at compile time (no runtime branch in the hot path). Override via
 * target_compile_definitions in CMakeLists.txt if a future board wires the
 * mono signal to the other slot.
 */
#define AUDIO_SLOT_LEFT   0
#define AUDIO_SLOT_RIGHT  1
#define AUDIO_SLOT_BOTH   2   /* TX only: duplicate the mono sample on L and R */

#ifndef AUDIO_RX_SLOT
#define AUDIO_RX_SLOT   AUDIO_SLOT_LEFT    /* which I2S slot to read for ADC input */
#endif

#ifndef AUDIO_TX_SLOT
#define AUDIO_TX_SLOT   AUDIO_SLOT_BOTH    /* which I2S slot(s) to write for DAC output */
#endif

/* ── I2S pinout ───────────────────────────────────────────────────────────── */
#define AUDIO_PIN_BCLK    10  /* I2S bit clock  (PIO side-set bit 0) */
#define AUDIO_PIN_LRCK    11  /* I2S word select (PIO side-set bit 1, = BCLK+1) */
#define AUDIO_PIN_DACDAT  9   /* I2S TX: data RP2350 -> ES8311 DSDIN (PCB: GPIO 9, hors du groupe BCLK/LRCK) */
#define AUDIO_PIN_ADCDAT  13  /* I2S RX: data ES8311 DO -> RP2350 */
#define AUDIO_PIN_MCLK    14  /* ES8311 master clock (12.5 MHz PWM) */

/* ── Callback type ────────────────────────────────────────────────────────── */

/*
 * audio_process_fn — callback called from the DMA IRQ for every buffer.
 *
 * rx : ADC input — AUDIO_BUFFER_FRAMES mono int16_t samples, already
 *      extracted from the AUDIO_RX_SLOT slot of the I2S wire frame.
 *      Holds audio data captured by the ES8311 (mic/line).
 *
 * tx : DAC output — AUDIO_BUFFER_FRAMES mono int16_t samples to fill.
 *      The HAL packs them onto AUDIO_TX_SLOT after the callback returns.
 *      Uninitialized on entry (may contain stale data).
 *
 * n_frames : always AUDIO_BUFFER_FRAMES.
 *
 * ctx : opaque pointer passed to audio_hal_init().
 *
 * ⚠ Real-time constraints:
 *   - Max duration: < 5.3 ms (time to read a half-buffer at 48 kHz)
 *   - No memory allocation
 *   - No printf(), sleep(), or blocking mutex
 *   - The IRQ takes priority — keep it short
 */
typedef void (*audio_process_fn)(
    const int16_t *rx,
    int16_t       *tx,
    uint32_t       n_frames,
    void          *ctx
);

/* ── API ──────────────────────────────────────────────────────────────────── */

/*
 * audio_hal_init() — prepares the audio engine (PIO, DMA) without starting it.
 *   fn  : audio processing callback (called from the DMA IRQ)
 *   ctx : opaque context passed to fn (e.g. pointer to the instrument)
 *
 * Must be called AFTER es8311_init() and BEFORE audio_hal_start().
 */
void audio_hal_init(audio_process_fn fn, void *ctx);

/*
 * audio_hal_start() — starts the DMA channels and the PIO state machines.
 * Must be called AFTER mclk_init() AND AFTER es8311_power_up().
 */
void audio_hal_start(void);

/*
 * mclk_init() — configures GPIO AUDIO_PIN_MCLK as a 12.5 MHz PWM output (50% duty).
 * Call AFTER es8311_init() to respect the EMI/I2C constraint.
 */
void mclk_init(void);

#ifdef __cplusplus
}
#endif
