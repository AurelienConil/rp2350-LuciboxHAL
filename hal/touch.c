/*
 * hal/touch.c — CY8CMBR3116 driver. See touch.h.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/platform.h"   /* __not_in_flash_func() */
#include "hardware/gpio.h"
#include "i2c_bus.h"
#include "touch.h"

/* ── Registers (TRM 001-91082, §1.5 Register Map) ─────────────────────────── */
#define REG_SENSOR_EN        0x00   /* 16 bit, little endian */
#define REG_SPO_CFG          0x4C
#define REG_CONFIG_CRC       0x7E   /* 16 bit */
#define REG_CTRL_CMD         0x86
#define REG_CTRL_CMD_ERR     0x89
#define REG_FAMILY_ID        0x8F   /* 154 for the CY8CMBR3xxx family */
#define REG_DEVICE_ID        0x90   /* 16 bit, 2565 for CY8CMBR3116 */
#define REG_CALC_CRC         0x94   /* 16 bit */
#define REG_TOTAL_WORKING    0x97   /* [7] SYSD_ERR, [4:0] sensors that passed */
#define REG_SNS_CP_HIGH      0x98   /* 16 bit: Cp > 45 pF */
#define REG_SNS_VDD_SHORT    0x9A   /* 16 bit */
#define REG_SNS_GND_SHORT    0x9C   /* 16 bit */
#define REG_SNS_SNS_SHORT    0x9E   /* 16 bit */
#define REG_CMOD_SHIELD      0xA0   /* [1] CMOD_LOW [0] CMOD_HIGH [2..4] shield */
#define REG_BUTTON_STAT      0xAA   /* 16 bit: bit n = CSn */

#define CMD_SAVE_CHECK_CRC   2
#define CMD_CALC_CRC         3
#define CMD_RESET            255

#define FAMILY_ID_EXPECTED   154
#define DEVICE_ID_3116       2565

/* Wanted configuration. SPO_CFG: [6:4] SPO1, [2:0] SPO0.
 * Function codes: 1 = capacitive sensor, 4 = host interrupt. */
#define WANT_SENSOR_EN       0xFFFFu
#define WANT_SPO_CFG         ((1u << 4) | 4u)   /* SPO1=CS15, SPO0=HI */

/* Safety poll if the IRQ edge was missed. */
#define POLL_SAFETY_MS       250

/* ── State ────────────────────────────────────────────────────────────────── */
static bool              present      = false;
static volatile bool     irq_pending  = true;   /* true: read once at start */
static uint16_t          state        = 0;
static bool              state_valid  = false;
static uint32_t          last_poll_ms = 0;
static volatile uint32_t irq_count    = 0;
static bool              warned_no_irq = false;

/* The SDK has ONE GPIO callback per core. Nothing else in this firmware uses
 * GPIO IRQs (encoders are decoded by a timer); if that changes, dispatch on
 * `gpio` here. Only sets a flag: the I2C read happens in the main loop. */
static void __not_in_flash_func(gpio_irq_cb)(uint gpio, uint32_t events) {
    (void)events;
    if (gpio == TOUCH_IRQ_PIN) { irq_pending = true; irq_count = irq_count + 1; }
}

/* ── I2C helpers ──────────────────────────────────────────────────────────────
 * The chip NACKs while it sleeps between scans and until it wakes up
 * (datasheet p.32): retry until ACK, ~2 ms apart. */

static bool wr(uint8_t reg, const uint8_t *data, size_t n, int tries) {
    uint8_t buf[4];
    if (n > 3) return false;
    buf[0] = reg;
    if (n) memcpy(buf + 1, data, n);
    for (int t = 0; t < tries; t++) {
        int r = i2c_write_timeout_us(LUCIBOX_I2C, TOUCH_I2C_ADDR, buf, n + 1, false, 5000);
        if (r == (int)(n + 1)) return true;
        sleep_ms(2);
    }
    return false;
}

static bool rd(uint8_t reg, uint8_t *data, size_t n, int tries) {
    for (int t = 0; t < tries; t++) {
        int r = i2c_write_timeout_us(LUCIBOX_I2C, TOUCH_I2C_ADDR, &reg, 1, true, 5000);
        if (r == 1) {
            r = i2c_read_timeout_us(LUCIBOX_I2C, TOUCH_I2C_ADDR, data, n, false, 5000);
            if (r == (int)n) return true;
        }
        sleep_ms(2);
    }
    return false;
}

static bool rd16(uint8_t reg, uint16_t *v, int tries) {
    uint8_t b[2];
    if (!rd(reg, b, 2, tries)) return false;
    *v = (uint16_t)(b[0] | (b[1] << 8));   /* little endian */
    return true;
}

static bool wr16(uint8_t reg, uint16_t v, int tries) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    return wr(reg, b, 2, tries);
}

/* Writes a command and waits for CTRL_CMD to return to 0 ("done"). */
static bool run_cmd(uint8_t cmd, int timeout_ms) {
    if (!wr(REG_CTRL_CMD, &cmd, 1, 50)) return false;
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        uint8_t v = 0xFF;
        if (rd(REG_CTRL_CMD, &v, 1, 1) && v == 0) return true;
        sleep_ms(5);
    }
    return false;
}

/* ── One-time chip configuration ──────────────────────────────────────────── */

static bool configure_if_needed(void) {
    uint16_t sensor_en;
    uint8_t  spo_cfg;
    if (!rd16(REG_SENSOR_EN, &sensor_en, 50)) return false;
    if (!rd(REG_SPO_CFG, &spo_cfg, 1, 50))    return false;

    if (sensor_en == WANT_SENSOR_EN && spo_cfg == WANT_SPO_CFG) {
        return true;   /* already configured: do NOT touch the flash */
    }

    printf("[TOUCH] Reconfiguring chip (SENSOR_EN 0x%04X->0x%04X, SPO_CFG 0x%02X->0x%02X)...\n",
           sensor_en, WANT_SENSOR_EN, spo_cfg, WANT_SPO_CFG);

    uint8_t spo = WANT_SPO_CFG;
    if (!wr16(REG_SENSOR_EN, WANT_SENSOR_EN, 50) || !wr(REG_SPO_CFG, &spo, 1, 50)) {
        printf("[TOUCH] config write failed\n");
        return false;
    }

    /* The CRC algorithm is not documented in the TRM: let the chip compute it
     * (CTRL_CMD 3 -> CALC_CRC), store it in CONFIG_CRC, then save (CTRL_CMD 2
     * verifies CONFIG_CRC == its own computation before writing flash). */
    uint16_t crc;
    if (!run_cmd(CMD_CALC_CRC, 200) || !rd16(REG_CALC_CRC, &crc, 50) ||
        !wr16(REG_CONFIG_CRC, crc, 50)) {
        printf("[TOUCH] CRC computation failed\n");
        return false;
    }

    if (!run_cmd(CMD_SAVE_CHECK_CRC, 1000)) {   /* ~220 ms typical */
        printf("[TOUCH] save command timed out\n");
        return false;
    }
    uint8_t err = 0xFF;
    if (!rd(REG_CTRL_CMD_ERR, &err, 1, 50) || err != 0) {
        printf("[TOUCH] save failed (CTRL_CMD_ERR=%u: 253=flash, 254=CRC)\n", err);
        return false;
    }

    /* New configuration takes effect after a reset. The chip may not ACK the
     * reset write itself: ignore its result, then wait for it to come back. */
    uint8_t rst = CMD_RESET;
    (void)wr(REG_CTRL_CMD, &rst, 1, 1);
    sleep_ms(50);
    printf("[TOUCH] Configuration saved, chip reset.\n");
    return true;
}

/* Boot-time self test of the chip (system diagnostics, TRM p.120-129). The
 * diagnostics run during the chip's ~900 ms boot: wait for the result. */
static void log_diagnostics(void) {
    uint8_t tw = 0;
    int waited = 0;
    while (waited < 1500) {
        if (rd(REG_TOTAL_WORKING, &tw, 1, 1) && (tw & 0x1F) != 0) break;
        sleep_ms(50);
        waited += 50;
    }
    uint8_t cnt = tw & 0x1F;
    printf("[TOUCH] self-test: %u/16 sensors OK%s (waited %d ms)\n",
           cnt, (tw & 0x80) ? ", SYSD_ERR set" : "", waited);
    if (cnt == 16 && !(tw & 0x80)) return;

    uint16_t cp = 0, vdd = 0, gnd = 0, sns = 0;
    uint8_t  cm = 0;
    (void)rd16(REG_SNS_CP_HIGH, &cp, 5);
    (void)rd16(REG_SNS_VDD_SHORT, &vdd, 5);
    (void)rd16(REG_SNS_GND_SHORT, &gnd, 5);
    (void)rd16(REG_SNS_SNS_SHORT, &sns, 5);
    (void)rd(REG_CMOD_SHIELD, &cm, 1, 5);
    printf("[TOUCH]   Cp>45pF      : 0x%04X  (bit n = pad n; trace too long / big pad)\n", cp);
    printf("[TOUCH]   short to VDD : 0x%04X\n", vdd);
    printf("[TOUCH]   short to GND : 0x%04X  (pad shorted to ground)\n", gnd);
    printf("[TOUCH]   short pad/pad: 0x%04X  (solder bridge between sensors/shield)\n", sns);
    printf("[TOUCH]   CMOD/shield  : 0x%02X  (bit0 CMOD too high, bit1 CMOD too low: "
           "needs 2.2 nF on CMOD)\n", cm);
    printf("[TOUCH]   failed sensors are DISABLED by the chip until its next reset\n");
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool touch_init(void) {
    present = false;

    /* The chip needs up to ~900 ms from power-up to its first scan, and I2C is
     * ready 15 ms after power: retry the ID read for up to ~1.5 s. */
    uint8_t family = 0;
    if (!rd(REG_FAMILY_ID, &family, 1, 300) || family != FAMILY_ID_EXPECTED) {
        printf("[TOUCH] CY8CMBR3116 not found at 0x%02X (family=%u) — touch disabled\n",
               TOUCH_I2C_ADDR, family);
        return false;
    }
    uint16_t dev_id = 0;
    if (rd16(REG_DEVICE_ID, &dev_id, 50) && dev_id != DEVICE_ID_3116) {
        printf("[TOUCH] warning: DEVICE_ID %u, expected %u (CY8CMBR3116)\n",
               dev_id, DEVICE_ID_3116);
    }

    if (!configure_if_needed()) {
        printf("[TOUCH] configuration failed — touch disabled\n");
        return false;
    }

    /* After a reset the chip is slow to answer again: confirm it's back. */
    if (!rd(REG_FAMILY_ID, &family, 1, 300) || family != FAMILY_ID_EXPECTED) {
        printf("[TOUCH] chip did not come back after reset — touch disabled\n");
        return false;
    }

    printf("[TOUCH] chip answered: family=%u (expected %u)\n", family, FAMILY_ID_EXPECTED);
    gpio_init(TOUCH_IRQ_PIN);
    gpio_set_dir(TOUCH_IRQ_PIN, GPIO_IN);
    gpio_pull_up(TOUCH_IRQ_PIN);   /* HI is open-drain, active low */
    irq_pending = true;
    gpio_set_irq_enabled_with_callback(TOUCH_IRQ_PIN, GPIO_IRQ_EDGE_FALL, true, gpio_irq_cb);

    present = true;
    uint16_t se = 0; uint8_t sp = 0;
    (void)rd16(REG_SENSOR_EN, &se, 5);
    (void)rd(REG_SPO_CFG, &sp, 1, 5);
    printf("[TOUCH] config in use: SENSOR_EN=0x%04X SPO_CFG=0x%02X\n", se, sp);
    log_diagnostics();
    uint16_t bs = 0;
    bool bs_ok = rd16(REG_BUTTON_STAT, &bs, 5);
    printf("[TOUCH] BUTTON_STAT=0x%04X (%s); IRQ pin GPIO%d idles %s\n", bs,
           bs_ok ? "read ok" : "READ FAILED", TOUCH_IRQ_PIN,
           gpio_get(TOUCH_IRQ_PIN) ? "HIGH (ok)" : "LOW <-- should idle high: "
           "HI not configured/wired, or held by chip");
    printf("[TOUCH] CY8CMBR3116 ready: 16 pads, IRQ on GPIO %d. Touch a pad to test; "
           "IRQ edges are counted.\n", TOUCH_IRQ_PIN);
    return true;
}

bool touch_poll(void) {
    if (!present) return false;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!irq_pending && (now - last_poll_ms) < POLL_SAFETY_MS) return state_valid;

    /* Clear BEFORE reading: a pulse arriving during the read re-arms it. */
    irq_pending = false;
    last_poll_ms = now;

    uint16_t s;
    if (rd16(REG_BUTTON_STAT, &s, 3)) {   /* ≤ ~6 ms worst case */
        if (s != state) {
            printf("[TOUCH] pads 0x%04X -> 0x%04X (IRQ edges so far: %u)\n",
                   state, s, (unsigned)irq_count);
            /* Pad changed but the HI line never pulsed: IRQ wiring/config issue
             * (the 250 ms safety poll caught it instead). */
            if (irq_count == 0 && !warned_no_irq) {
                warned_no_irq = true;
                printf("[TOUCH] WARNING: pad change seen but NO IRQ edge on GPIO%d yet: "
                       "check pin 23 (HI) -> GPIO%d wiring; falling back to polling.\n",
                       TOUCH_IRQ_PIN, TOUCH_IRQ_PIN);
            }
        }
        state = s;
        state_valid = true;
    } else {
        static uint32_t last_err_ms = 0;
        if (now - last_err_ms > 2000) {
            last_err_ms = now;
            printf("[TOUCH] read of BUTTON_STAT failed (NACK/timeout) — retrying\n");
        }
        irq_pending = true;   /* retry next tick */
    }
    return state_valid;
}

bool touch_present(void) {
    return present;
}

uint16_t touch_state(void) {
    return state;
}
