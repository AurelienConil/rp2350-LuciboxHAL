/*
 * hal/controls.h — Hardware controls of the Lucibox PCB
 *
 *   8 pots            : RP2350B internal ADC, GPIO 40-47 (A0..A7)
 *   6 simple buttons  : GPIO 26-31, pull-up, active-low
 *   4 lit buttons     : GPIO 32/34/36/38 (switch) + 33/35/37/39 (LED)
 *   4 encoders        : GPIO 18-25 (A,B pairs), no push switch
 *   MPR121 (stub)     : I2C1 addr 0x5A
 *
 * (The WS2812 strip is an output, not a control: it is not handled here.)
 *
 * ── Rules ───────────────────────────────────────────────────────────────────
 * The audio side (process()) ONLY calls the *_get / *_pressed / *_released /
 * *_delta functions below. All of them are O(1), lock-free, never touch the
 * hardware (except controls_led_set, a single atomic GPIO write), and are safe
 * from the audio IRQ. Never call controls_pot_read_raw()/_norm() from
 * process(): unfiltered, triggers a real ADC conversion.
 *
 * Scanning is decoupled from the audio IRQ:
 *   - controls_update()   : main() loop, every CONTROLS_UPDATE_PERIOD_MS
 *                           (pots, buttons, MPR121 — I2C blocking is OK there)
 *   - encoder decoder     : 1 kHz repeating timer started by controls_init()
 *                           (50 Hz would lose steps). Reads 8 GPIOs, nothing
 *                           blocking.
 *
 * ── Single-consumer assumption ──────────────────────────────────────────────
 * Edge events and encoder deltas are "consume on read" with state kept on the
 * reader side: exactly ONE context (the instrument's process()) must read them.
 * Level getters (controls_pot_get, controls_button_get) have no such limit.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Control-rate period of controls_update(), in ms (~50 Hz). */
#define CONTROLS_UPDATE_PERIOD_MS  20

/* ── Potentiometers ──────────────────────────────────────────────────────── */

#define CONTROLS_NUM_POTS  8   /* A0..A7 = GPIO 40..47 = ADC ch 0..7 */

/* Call once at startup, after stdio_init_all(). Initialises everything
 * below (ADC, GPIOs, encoder timer). */
void controls_init(void);

/* Cached, smoothed pot value 0.0..1.0. ch: 0..CONTROLS_NUM_POTS-1.
 * The only pot call to use inside process(). Out of range -> 0.0f. */
float controls_pot_get(uint8_t ch);

/* Direct unfiltered ADC conversion (12 bit). NOT for process() — calibration /
 * main-loop use only. */
float    controls_pot_read_norm(uint8_t ch);
uint16_t controls_pot_read_raw(uint8_t ch);

/* ── Buttons ─────────────────────────────────────────────────────────────────
 * One index space, 0..CONTROLS_NUM_BUTTONS-1:
 *   0..5  simple buttons (GPIO 26..31)           -> CONTROLS_BTN_SIMPLE(n)
 *   6..9  lit buttons    (GPIO 32,34,36,38)      -> CONTROLS_BTN_LIT(n)
 * Software-debounced (CONTROLS_BUTTON_DEBOUNCE_SAMPLES consecutive reads).
 * Polarity is hidden: true == pressed. */

#define CONTROLS_NUM_BUTTONS_SIMPLE  6
#define CONTROLS_NUM_BUTTONS_LIT     4
#define CONTROLS_NUM_BUTTONS  (CONTROLS_NUM_BUTTONS_SIMPLE + CONTROLS_NUM_BUTTONS_LIT)

#define CONTROLS_BTN_SIMPLE(n)  (n)
#define CONTROLS_BTN_LIT(n)     (CONTROLS_NUM_BUTTONS_SIMPLE + (n))

/* Debounced level: true while held. Out of range -> false. */
bool controls_button_get(uint8_t idx);

/* Edge events: true ONCE per press / release (multiple unread events are
 * returned one per call, none lost up to 255 pending). Poll every block. */
bool controls_button_pressed(uint8_t idx);
bool controls_button_released(uint8_t idx);

/* ── LEDs of the lit buttons ─────────────────────────────────────────────────
 * n: 0..CONTROLS_NUM_BUTTONS_LIT-1 (LED of CONTROLS_BTN_LIT(n)).
 * Single atomic GPIO write: safe from process() as well as main(). */
#define CONTROLS_NUM_LEDS  CONTROLS_NUM_BUTTONS_LIT
void controls_led_set(uint8_t n, bool on);
bool controls_led_get(uint8_t n);

/* ── Encoders (quadrature, no push) ──────────────────────────────────────────
 * idx: 0..CONTROLS_NUM_ENCODERS-1, pins (A,B) = (18,19) (20,21) (22,23) (24,25).
 * Clockwise = positive. Swap the A/B pins in controls.c (or flip
 * CONTROLS_ENCODER_INVERT) if the mechanics turn out reversed. */
#define CONTROLS_NUM_ENCODERS  4

/* Detents turned since the previous call for this encoder (signed).
 * Typical use:  value += controls_encoder_delta(0) * step; */
int32_t controls_encoder_delta(uint8_t idx);

/* Absolute detent count since boot (never consumed — for display/debug). */
int32_t controls_encoder_position(uint8_t idx);

/* ── Scan ───────────────────────────────────────────────────────────────────── */

/* Scans pots + buttons + MPR121 once. Called by main() every
 * CONTROLS_UPDATE_PERIOD_MS. NEVER from the audio IRQ, never by instruments. */
void controls_update(void);

/* ── MPR121 (12-pad capacitive touch, I2C1 addr 0x5A) — STUB ─────────────────
 * Shares I2C1 with the ES8311. Blocking I2C: only reachable via
 * controls_update(). */
#define CONTROLS_MPR121_I2C_ADDR  0x5A
#define CONTROLS_MPR121_NUM_PADS  12

bool     controls_mpr121_init(void);     /* false if absent; call after es8311_init() */
void     controls_mpr121_update(void);   /* called by controls_update() */
uint16_t controls_mpr121_touched(void);  /* 12-bit mask, safe from IRQ */

#ifdef __cplusplus
}
#endif
