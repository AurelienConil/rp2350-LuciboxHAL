/*
 * hal/controls.h — Hardware controls: potentiometers + MPR121 (stub)
 *
 * Potentiometers: RP2350 internal ADC (GPIO 26-29, 12-bit)
 * Capacitive touch: MPR121 on I2C1 (addr 0x5A) — stub, to be implemented
 *
 * Pot reading is mechanically control-rate: hal/audio.c calls
 * controls_update() once per audio block (from dma_irq_handler, before the
 * instrument's process()) — this reads all channels and applies a one-pole
 * smoothing filter into a cache. Instruments must read controls_pot_get()
 * (O(1), no ADC access) and must NEVER call controls_pot_read_raw()/_norm()
 * from process(): those trigger a real, unfiltered ADC conversion and would
 * defeat the "once per block, smoothed" discipline (zipper noise, wasted
 * cycles if called per-sample).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Potentiometers (RP2350 internal ADC) ────────────────────────────────── */

/*
 * Number of available ADC channels.
 * GPIO 26 -> channel 0, GPIO 27 -> channel 1, GPIO 28 -> channel 2, GPIO 29 -> channel 3.
 * Actual wiring and number of pots to be defined per the final hardware.
 */
#define CONTROLS_NUM_POTS  4

/*
 * controls_init() — initializes the internal ADC and the pot GPIOs.
 * Call once at startup, after stdio_init_all().
 */
void controls_init(void);

/*
 * controls_pot_read_norm() — reads a potentiometer, returns 0.0..1.0
 * ch: 0..CONTROLS_NUM_POTS-1 (0 = GPIO 26, 1 = GPIO 27, etc.)
 * Resolution: 12 bits (4096 levels). Unfiltered, triggers a real conversion.
 * ⚠ Do NOT call from process()/the audio IRQ — use controls_pot_get() instead
 *   (cached, smoothed, updated once per block by the HAL). Reserved for
 *   one-off reads (calibration, main-loop UI). Not thread-safe if called
 *   from multiple contexts simultaneously.
 */
float controls_pot_read_norm(uint8_t ch);

/*
 * controls_pot_read_raw() — same as controls_pot_read_norm(), raw 0..4095.
 * Same restriction: not for use inside process(), see controls_pot_get().
 */
uint16_t controls_pot_read_raw(uint8_t ch);

/*
 * controls_update() — reads all CONTROLS_NUM_POTS channels and updates the
 * smoothed cache (one-pole filter). Called automatically ONCE PER AUDIO
 * BLOCK by the HAL (hal/audio.c, dma_irq_handler), before the instrument's
 * process(). Instruments must NOT call this themselves.
 */
void controls_update(void);

/*
 * controls_pot_get() — cached, smoothed pot value, 0.0..1.0.
 * O(1), no ADC access — safe to call as often as needed from process()
 * (the underlying conversion already happened once this block, in
 * controls_update()). This is the ONLY pot-reading call an instrument
 * should make from process().
 * ch: 0..CONTROLS_NUM_POTS-1. Returns 0.0f if out of range.
 */
float controls_pot_get(uint8_t ch);

/* ── MPR121 (12-pad capacitive touch sensor, I2C addr 0x5A) ─────────────────
 *
 * ⚠ STUB — to be implemented. The interface is defined to allow future
 *   integration without modifying the instruments.
 *
 * Shares the I2C1 bus with the ES8311 (addr 0x18) — compatible.
 * Constraint: do not access the MPR121 from the DMA IRQ (I2C is blocking).
 *   Use a polling task in the main loop.
 */

#define CONTROLS_MPR121_I2C_ADDR  0x5A
#define CONTROLS_MPR121_NUM_PADS  12

/*
 * controls_mpr121_init() — initializes the MPR121. Returns false if absent.
 * Call after es8311_init() (shares I2C1).
 */
bool controls_mpr121_init(void);

/*
 * controls_mpr121_update() — reads the state of the 12 pads. Call from the main loop.
 * Updates an internal state accessible via controls_mpr121_touched().
 */
void controls_mpr121_update(void);

/*
 * controls_mpr121_touched() — returns a 12-bit mask of the active pads.
 * bit N = 1 -> pad N is touched.
 * Safe to call from the IRQ (atomic read of the state updated by update).
 */
uint16_t controls_mpr121_touched(void);

#ifdef __cplusplus
}
#endif
