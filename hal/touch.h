/*
 * hal/touch.h — CY8CMBR3116 capacitive touch controller (16 pads), I2C
 *
 * Datasheet 001-85330, Registers TRM 001-91082. Address 0x37 (factory default)
 * on the shared bus (hal/i2c_bus.h). Host interrupt "HI" (active-low pulse,
 * 200..700 us on every sensor-state change) -> GPIO TOUCH_IRQ_PIN.
 *
 * Pad n = sensor CSn (n = 0..15) = bit n of BUTTON_STAT.
 *
 * ── Factory configuration vs. this PCB ──────────────────────────────────────
 * Factory default (TRM p.20): only CS0..CS7 enabled, pin 9 (CS15/SH/HI) is the
 * HI output, pin 23 (HI/BUZ/GPO7) is GPO7. On this PCB the IRQ is wired to
 * pin 23 and we want all 16 pads, so touch_init() reconfigures the chip once:
 *   SENSOR_EN (0x00) = 0xFFFF   all 16 sensors
 *   SPO_CFG   (0x4C) = 0x14     SPO1 = capacitive sensor (CS15, pin 9)
 *                               SPO0 = host interrupt   (pin 23)
 * then computes the CRC (CTRL_CMD 3), stores it, saves to the chip's flash
 * (CTRL_CMD 2, ~220 ms) and resets it (CTRL_CMD 255). This is done ONLY if the
 * chip does not already hold this configuration (flash: 100 k write cycles —
 * never written at every boot).
 *
 * Hardware reminders (datasheet p.10): the CS6 pad (pin 18) doubles as the
 * chip's reset line during boot-up: it must NOT be grounded at power-up.
 *
 * Blocking I2C: touch_init() at boot, touch_poll() from the main loop only.
 * Never from the audio IRQ.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_I2C_ADDR   0x37
#define TOUCH_IRQ_PIN    6
#define TOUCH_NUM_PADS   16

/* Presence check + one-time configuration + IRQ setup. Call once, after the
 * I2C bus is initialised. Returns false if the chip does not answer (touch is
 * then disabled; touch_poll() does nothing). May block up to ~1.5 s when the
 * chip is absent, ~1 s the very first time (flash save + reset). */
bool touch_init(void);

/* Refreshes the cached pad state from BUTTON_STAT if the IRQ fired (or as a
 * ~250 ms safety poll). Main loop only. Returns true if the cached state is
 * valid (chip present and the last read succeeded). */
bool touch_poll(void);

/* True once touch_init() succeeded. */
bool touch_present(void);

/* Cached 16-bit mask: bit n set = pad n touched (debounced by the chip). */
uint16_t touch_state(void);

#ifdef __cplusplus
}
#endif
