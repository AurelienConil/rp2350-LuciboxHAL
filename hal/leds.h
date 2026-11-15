/*
 * hal/leds.h — WS2812 strip (16 LEDs, data on GPIO 1), PIO1 SM (claimed).
 *
 * Split in two on purpose:
 *   leds_set()/leds_clear()  : write the framebuffer. O(1), no hardware —
 *                              safe from process() (audio IRQ).
 *   leds_show()              : pushes the framebuffer to the strip. Blocks
 *                              ~0.5 ms (16 FIFO writes) — main loop ONLY;
 *                              main() calls it once per control tick (50 Hz).
 *
 * Colours are 0..255 per channel, scaled by LEDS_BRIGHTNESS at show time.
 * Full-white 16 LEDs can draw ~1 A: keep the default cap when on USB power.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEDS_PIN         1
#define LEDS_COUNT       16
#define LEDS_BRIGHTNESS  64    /* global cap, 0..255 */

void leds_init(void);
void leds_set(uint8_t i, uint8_t r, uint8_t g, uint8_t b);
void leds_clear(void);
void leds_show(void);

#ifdef __cplusplus
}
#endif
