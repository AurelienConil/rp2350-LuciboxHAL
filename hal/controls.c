/*
 * hal/controls.c — Pots, buttons, lit-button LEDs, encoders, touch pads
 * See controls.h for the API contract and pinout.
 */

#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/platform.h"   /* __not_in_flash_func() */
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "controls.h"
#include "touch.h"

/* ── Pinout (Lucibox PCB) ─────────────────────────────────────────────────── */

/* A0..A7 = GPIO 40..47 = ADC channels 0..7 (RP2350B only — needs a board
 * definition with PICO_RP2350A=0, see CLAUDE.md). */
#define CONTROLS_ADC_BASE_PIN  40

/* Button index -> GPIO. 0..5 simple, 6..9 lit. */
static const uint8_t BUTTON_PINS[CONTROLS_NUM_BUTTONS_GPIO] = {
    26, 27, 28, 29, 30, 31,     /* simple (assumed contiguous D26..D31) */
    32, 34, 36, 38              /* lit: switch pins */
};

/* LED n (of lit button n) -> GPIO. */
static const uint8_t LED_PINS[CONTROLS_NUM_LEDS] = { 33, 35, 37, 39 };

/* Encoder idx -> {A, B} GPIO. */
static const uint8_t ENCODER_PINS[CONTROLS_NUM_ENCODERS][2] = {
    {18, 19}, {20, 21}, {22, 23}, {24, 25}
};

/* Quadrature transitions per mechanical detent (4 for most, 2 for some).
 * CONTROLS_ENCODER_INVERT 1 reverses the direction globally. */
#ifndef CONTROLS_ENCODER_STEPS_PER_DETENT
#define CONTROLS_ENCODER_STEPS_PER_DETENT 4
#endif
#ifndef CONTROLS_ENCODER_INVERT
#define CONTROLS_ENCODER_INVERT 0
#endif

/* ── Potentiometers ───────────────────────────────────────────────────────── */

static float pot_smoothed[CONTROLS_NUM_POTS];

/* One-pole smoothing: y += coeff * (x - y), coeff derived from a time
 * constant so it stays right if CONTROLS_UPDATE_PERIOD_MS changes. */
#define CONTROLS_POT_SMOOTH_TIME_MS 15.0f
static float pot_smooth_coeff = 0.3f;

uint16_t controls_pot_read_raw(uint8_t ch) {
    if (ch >= CONTROLS_NUM_POTS) return 0;
    adc_select_input(ch);
    return adc_read();  /* 0..4095 */
}

float controls_pot_read_norm(uint8_t ch) {
    return controls_pot_read_raw(ch) / 4095.0f;
}

float controls_pot_get(uint8_t ch) {
    if (ch >= CONTROLS_NUM_POTS) return 0.0f;
    return pot_smoothed[ch];
}

/* ── Buttons ──────────────────────────────────────────────────────────────── */

/* 2 samples @ 20 ms = ~40 ms worst-case debounce latency. */
#define CONTROLS_BUTTON_DEBOUNCE_SAMPLES 2

typedef struct {
    bool    candidate;   /* last raw reading, awaiting confirmation */
    uint8_t count;       /* consecutive identical candidate reads */
} button_filter_t;

static button_filter_t button_filter[CONTROLS_NUM_BUTTONS];

/* Shared main -> audio. Each variable has ONE writer:
 *  - debounced / press_count / release_count : written by controls_update()
 *  - press_seen / release_seen               : written by the reader only
 * so no read-modify-write race between main and the audio IRQ. */
static volatile bool    button_debounced[CONTROLS_NUM_BUTTONS];
static volatile uint8_t press_count[CONTROLS_NUM_BUTTONS];
static volatile uint8_t release_count[CONTROLS_NUM_BUTTONS];
static uint8_t          press_seen[CONTROLS_NUM_BUTTONS];
static uint8_t          release_seen[CONTROLS_NUM_BUTTONS];

bool controls_button_get(uint8_t idx) {
    if (idx >= CONTROLS_NUM_BUTTONS) return false;
    return button_debounced[idx];
}

bool controls_button_pressed(uint8_t idx) {
    if (idx >= CONTROLS_NUM_BUTTONS) return false;
    if (press_count[idx] == press_seen[idx]) return false;
    press_seen[idx]++;
    return true;
}

bool controls_button_released(uint8_t idx) {
    if (idx >= CONTROLS_NUM_BUTTONS) return false;
    if (release_count[idx] == release_seen[idx]) return false;
    release_seen[idx]++;
    return true;
}

static void buttons_init_(void) {
    for (uint8_t i = 0; i < CONTROLS_NUM_BUTTONS_GPIO; i++) {
        gpio_init(BUTTON_PINS[i]);
        gpio_set_dir(BUTTON_PINS[i], GPIO_IN);
        gpio_pull_up(BUTTON_PINS[i]);
    }
    sleep_ms(10);  /* laisser le pull-up charger l'éventuel RC du PCB avant la 1re lecture */
    /* Prime with an immediate reading: no phantom press event at boot. */
    for (uint8_t i = 0; i < CONTROLS_NUM_BUTTONS_GPIO; i++) {
        bool pressed = !gpio_get(BUTTON_PINS[i]);  /* active-low */
        button_filter[i].candidate = pressed;
        button_filter[i].count     = CONTROLS_BUTTON_DEBOUNCE_SAMPLES;
        button_debounced[i]        = pressed;
    }
}

/* ── LEDs ─────────────────────────────────────────────────────────────────── */

void controls_led_set(uint8_t n, bool on) {
    if (n >= CONTROLS_NUM_LEDS) return;
    gpio_put(LED_PINS[n], on);
}

bool controls_led_get(uint8_t n) {
    if (n >= CONTROLS_NUM_LEDS) return false;
    return gpio_get_out_level(LED_PINS[n]);
}

static void leds_init_(void) {
    for (uint8_t n = 0; n < CONTROLS_NUM_LEDS; n++) {
        gpio_init(LED_PINS[n]);
        gpio_set_dir(LED_PINS[n], GPIO_OUT);
        gpio_put(LED_PINS[n], 0);
        /* Garde-fou : LED sans résistance dédiée possible → limite l'étage de
         * sortie à 2 mA (≠ vraie limitation de courant, mais protège en pratique). */
        gpio_set_drive_strength(LED_PINS[n], GPIO_DRIVE_STRENGTH_2MA);
    }
}

/* ── Encoders ─────────────────────────────────────────────────────────────── */

/* Decoded by a 1 kHz repeating timer (a 50 Hz scan would lose steps).
 * enc_pos has ONE writer (the timer callback); enc_last is reader-only. */
static volatile int32_t enc_pos[CONTROLS_NUM_ENCODERS];
static int32_t          enc_last[CONTROLS_NUM_ENCODERS];
static uint8_t          enc_prev[CONTROLS_NUM_ENCODERS];  /* (A<<1)|B */
static int8_t           enc_sub[CONTROLS_NUM_ENCODERS];   /* transitions toward next detent */

/* Index = (prev<<2)|cur ; +1 / -1 for a valid step, 0 for no move / bounce. */
static const int8_t ENC_TABLE[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

static inline uint8_t enc_read_(uint8_t i) {
    return (uint8_t)((gpio_get(ENCODER_PINS[i][0]) << 1) | gpio_get(ENCODER_PINS[i][1]));
}

static bool __not_in_flash_func(encoder_timer_cb)(struct repeating_timer *t) {
    (void)t;
    for (uint8_t i = 0; i < CONTROLS_NUM_ENCODERS; i++) {
        uint8_t cur  = enc_read_(i);
        int8_t  step = ENC_TABLE[(enc_prev[i] << 2) | cur];
        enc_prev[i]  = cur;
        if (step == 0) continue;
#if CONTROLS_ENCODER_INVERT
        step = -step;
#endif
        enc_sub[i] += step;
        if (enc_sub[i] >= CONTROLS_ENCODER_STEPS_PER_DETENT) {
            enc_sub[i] = 0;
            enc_pos[i] = enc_pos[i] + 1;
        } else if (enc_sub[i] <= -CONTROLS_ENCODER_STEPS_PER_DETENT) {
            enc_sub[i] = 0;
            enc_pos[i] = enc_pos[i] - 1;
        }
    }
    return true;  /* keep repeating */
}

int32_t controls_encoder_delta(uint8_t idx) {
    if (idx >= CONTROLS_NUM_ENCODERS) return 0;
    int32_t now = enc_pos[idx];
    int32_t d   = now - enc_last[idx];
    enc_last[idx] = now;
    return d;
}

int32_t controls_encoder_position(uint8_t idx) {
    if (idx >= CONTROLS_NUM_ENCODERS) return 0;
    return enc_pos[idx];
}

static struct repeating_timer encoder_timer;

static void encoders_init_(void) {
    for (uint8_t i = 0; i < CONTROLS_NUM_ENCODERS; i++) {
        for (uint8_t k = 0; k < 2; k++) {
            gpio_init(ENCODER_PINS[i][k]);
            gpio_set_dir(ENCODER_PINS[i][k], GPIO_IN);
            gpio_pull_up(ENCODER_PINS[i][k]);
        }
        enc_prev[i] = enc_read_(i);
    }
    add_repeating_timer_us(-1000, encoder_timer_cb, NULL, &encoder_timer);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void controls_init(void) {
    adc_init();
    for (int ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        adc_gpio_init(CONTROLS_ADC_BASE_PIN + ch);
    }
    pot_smooth_coeff = 1.0f - expf(-(float)CONTROLS_UPDATE_PERIOD_MS / CONTROLS_POT_SMOOTH_TIME_MS);

    /* Prime the cache so controls_pot_get() never returns a stale 0.0f. */
    for (int ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        pot_smoothed[ch] = controls_pot_read_norm(ch);
    }

    buttons_init_();
    leds_init_();
    encoders_init_();
#if CONTROLS_TOUCH_ENABLED
    touch_init();   /* I2C bus must already be up; false = absent, pads stay released */
#endif

    /* Bring-up dump: catches unplugged/floating pots and stuck buttons. */
    printf("[CONTROLS] pot raw (0..4095):");
    for (int ch = 0; ch < CONTROLS_NUM_POTS; ch++) printf(" A%d=%u", ch, controls_pot_read_raw(ch));
    printf("\n[CONTROLS] button levels at boot (1 = released, pull-up):");
    for (uint8_t i = 0; i < CONTROLS_NUM_BUTTONS_GPIO; i++)
        printf(" b%u(GPIO%u)=%d", i, BUTTON_PINS[i], gpio_get(BUTTON_PINS[i]));
    printf("\n[CONTROLS] encoder pins (A,B) at boot:");
    for (uint8_t i = 0; i < CONTROLS_NUM_ENCODERS; i++)
        printf(" e%u=%d%d", i, gpio_get(ENCODER_PINS[i][0]), gpio_get(ENCODER_PINS[i][1]));
    printf("\n");

    printf("[CONTROLS] %d pots (GPIO %d..%d), %d buttons (%d simple + %d lit + %d touch), "
           "%d LEDs, %d encoders\n",
           CONTROLS_NUM_POTS, CONTROLS_ADC_BASE_PIN,
           CONTROLS_ADC_BASE_PIN + CONTROLS_NUM_POTS - 1,
           CONTROLS_NUM_BUTTONS, CONTROLS_NUM_BUTTONS_SIMPLE, CONTROLS_NUM_BUTTONS_LIT,
           CONTROLS_NUM_BUTTONS_TOUCH,
           CONTROLS_NUM_LEDS, CONTROLS_NUM_ENCODERS);
}

/* ── Control-rate scan (main loop, never the audio IRQ) ───────────────────── */

void controls_update(void) {
    for (uint8_t ch = 0; ch < CONTROLS_NUM_POTS; ch++) {
        float raw = controls_pot_read_norm(ch);
        pot_smoothed[ch] += pot_smooth_coeff * (raw - pot_smoothed[ch]);
    }

    for (uint8_t i = 0; i < CONTROLS_NUM_BUTTONS_GPIO; i++) {
        bool raw_pressed = !gpio_get(BUTTON_PINS[i]);  /* active-low */
        button_filter_t *f = &button_filter[i];
        if (raw_pressed == f->candidate) {
            if (f->count < CONTROLS_BUTTON_DEBOUNCE_SAMPLES) f->count++;
        } else {
            f->candidate = raw_pressed;
            f->count     = 1;
        }
        if (f->count >= CONTROLS_BUTTON_DEBOUNCE_SAMPLES && f->candidate != button_debounced[i]) {
            button_debounced[i] = f->candidate;
            if (f->candidate) press_count[i]   = press_count[i] + 1;
            else              release_count[i] = release_count[i] + 1;
        }
    }

    /* Touch pads: the chip debounces, so its state is already clean. Edges are
     * derived here (the single writer of the press/release counters). */
#if CONTROLS_TOUCH_ENABLED
    if (touch_poll()) {
        static uint16_t touch_prev = 0;
        uint16_t now = touch_state();
        uint16_t changed = now ^ touch_prev;
        for (uint8_t n = 0; n < CONTROLS_NUM_BUTTONS_TOUCH; n++) {
            if (!(changed & (1u << n))) continue;
            uint8_t idx = CONTROLS_BTN_TOUCH(n);
            bool down = (now >> n) & 1u;
            button_debounced[idx] = down;
            if (down) press_count[idx]   = press_count[idx] + 1;
            else      release_count[idx] = release_count[idx] + 1;
        }
        touch_prev = now;
    }
#endif
}

int controls_button_gpio(uint8_t idx) {
    return idx < CONTROLS_NUM_BUTTONS_GPIO ? (int)BUTTON_PINS[idx] : -1;
}
