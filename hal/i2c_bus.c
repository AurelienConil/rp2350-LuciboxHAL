/*
 * hal/i2c_bus.c — shared I2C bus: init, diagnostics, scan. See i2c_bus.h.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "i2c_bus.h"

typedef struct {
    uint8_t     addr;
    uint32_t    mask;
    const char *name;
    const char *hint;
} known_dev_t;

static const known_dev_t KNOWN[] = {
    { I2C_ADDR_ES8311,    I2C_FOUND_ES8311,    "ES8311 codec",
      "check CE/AD0 pin (addr 0x18 = tied to GND), power, SDA/SCL wiring" },
    { I2C_ADDR_TPA6130A2, I2C_FOUND_TPA6130A2, "TPA6130A2 headphone amp",
      "its SD pin must be HIGH (hardware shutdown = no I2C), VDD 2.5-5.5 V, addr 0x60" },
    { I2C_ADDR_TOUCH,     I2C_FOUND_TOUCH,     "CY8CMBR3116 touch",
      "boots in up to ~900 ms; CS6 pad (pin 18) must not be grounded at power-up; "
      "default addr 0x37; CMOD 2.2 nF + VCC 0.1 uF required" },
};
#define N_KNOWN (sizeof(KNOWN) / sizeof(KNOWN[0]))

bool i2c_bus_probe(uint8_t addr, int tries) {
    uint8_t d;
    for (int t = 0; t < tries; t++) {
        if (i2c_read_timeout_us(LUCIBOX_I2C, addr, &d, 1, false, 5000) >= 0) return true;
        sleep_ms(3);
    }
    return false;
}

bool i2c_bus_init(void) {
    printf("[I2C] bus I2C0: SDA=GPIO%d SCL=GPIO%d @ %d kHz\n",
           LUCIBOX_I2C_SDA, LUCIBOX_I2C_SCL, LUCIBOX_I2C_HZ / 1000);

    /* Idle level with the internal pull-ups only: both must read 1. A 0 means
     * a short to GND, a chip holding the line, or SDA/SCL swapped with a pin
     * driven elsewhere. (External 2.2-4.7k pull-ups recommended for 100 kHz.) */
    gpio_init(LUCIBOX_I2C_SDA); gpio_set_dir(LUCIBOX_I2C_SDA, GPIO_IN); gpio_pull_up(LUCIBOX_I2C_SDA);
    gpio_init(LUCIBOX_I2C_SCL); gpio_set_dir(LUCIBOX_I2C_SCL, GPIO_IN); gpio_pull_up(LUCIBOX_I2C_SCL);
    sleep_ms(2);
    bool sda = gpio_get(LUCIBOX_I2C_SDA);
    bool scl = gpio_get(LUCIBOX_I2C_SCL);
    printf("[I2C] idle levels: SDA=%d SCL=%d %s\n", sda, scl,
           (sda && scl) ? "(ok)" : "<-- PROBLEM");

    if (!scl) {
        printf("[I2C] ERROR: SCL stuck LOW: short to GND, or a chip holding the clock. "
               "I2C unusable.\n");
        return false;
    }
    if (!sda) {
        /* A chip may be stuck mid-byte (e.g. after a reset in the middle of a
         * transfer): clock SCL up to 9 times until it releases SDA. */
        printf("[I2C] SDA stuck LOW: attempting 9-clock bus recovery...\n");
        gpio_set_dir(LUCIBOX_I2C_SCL, GPIO_OUT);
        for (int i = 0; i < 9 && !gpio_get(LUCIBOX_I2C_SDA); i++) {
            gpio_put(LUCIBOX_I2C_SCL, 0); sleep_us(10);
            gpio_put(LUCIBOX_I2C_SCL, 1); sleep_us(10);
        }
        gpio_set_dir(LUCIBOX_I2C_SCL, GPIO_IN);
        sda = gpio_get(LUCIBOX_I2C_SDA);
        printf("[I2C] after recovery: SDA=%d %s\n", sda,
               sda ? "(released, ok)" : "<-- still stuck: short to GND / chip dead");
        if (!sda) return false;
    }

    i2c_init(LUCIBOX_I2C, LUCIBOX_I2C_HZ);
    gpio_set_function(LUCIBOX_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(LUCIBOX_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(LUCIBOX_I2C_SDA);
    gpio_pull_up(LUCIBOX_I2C_SCL);
    sleep_ms(10);
    printf("[I2C] peripheral initialised\n");
    return true;
}

uint32_t i2c_bus_scan(void) {
    printf("[I2C] scanning 0x08..0x77 ...\n");

    uint32_t found = 0;
    int unexpected = 0;
    bool seen[128] = { false };

    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_bus_probe(a, 1)) seen[a] = true;
    }

    /* Expected chips get extra retries (the touch chip NACKs while asleep and
     * during its ~900 ms boot, the TPA only answers once SD is high). */
    for (unsigned i = 0; i < N_KNOWN; i++) {
        if (!seen[KNOWN[i].addr]) seen[KNOWN[i].addr] = i2c_bus_probe(KNOWN[i].addr, 20);
    }

    for (unsigned i = 0; i < N_KNOWN; i++) {
        if (seen[KNOWN[i].addr]) {
            found |= KNOWN[i].mask;
            printf("[I2C]   0x%02X  %-24s found\n", KNOWN[i].addr, KNOWN[i].name);
        } else {
            printf("[I2C]   0x%02X  %-24s MISSING  -> %s\n",
                   KNOWN[i].addr, KNOWN[i].name, KNOWN[i].hint);
        }
    }
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (!seen[a]) continue;
        bool known = false;
        for (unsigned i = 0; i < N_KNOWN; i++) if (KNOWN[i].addr == a) known = true;
        if (!known) {
            printf("[I2C]   0x%02X  (unexpected device)\n", a);
            unexpected++;
        }
    }
    if (unexpected > 4) {
        printf("[I2C] many devices answer: probably SDA stuck/floating (bus noise), "
               "not real chips\n");
    }
    printf("[I2C] scan done: %d/%u expected chips present\n",
           __builtin_popcount(found), (unsigned)N_KNOWN);
    return found;
}
