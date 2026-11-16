/*
 * hal/i2c_bus.h — The board's single shared I2C bus
 *
 * One bus for every I2C chip of the PCB:
 *   ES8311        0x18  codec (hal/es8311.c)
 *   TPA6130A2     0x60  headphone amplifier (no driver yet: presence probe only;
 *                       answers only while its SD pin is high)
 *   CY8CMBR3116   0x37  capacitive touch controller (hal/touch.c)
 *
 * GPIO 16 (SDA) / GPIO 17 (SCL) belong to the RP2350's I2C0 peripheral
 * (SDA = GPIO 0,4,8,...,16,..  SCL = GPIO 1,5,9,...,17,..). The pinout moved
 * from GPIO 6/7 (I2C1) on the previous PCB revision.
 *
 * 100 kHz: required while MCLK is active (see hal/es8311.h). Blocking I2C:
 * main loop / init only, never from the audio IRQ.
 *
 * Bring-up: i2c_bus_init() logs the idle level of both lines (missing
 * pull-ups / shorts show up there) and i2c_bus_scan() logs which chips answer.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUCIBOX_I2C        i2c0
#define LUCIBOX_I2C_SDA    16
#define LUCIBOX_I2C_SCL    17
#define LUCIBOX_I2C_HZ     100000

#define I2C_ADDR_ES8311    0x18
#define I2C_ADDR_TPA6130A2 0x60
#define I2C_ADDR_TOUCH     0x37

/* Bits of the mask returned by i2c_bus_scan(). */
#define I2C_FOUND_ES8311   (1u << 0)
#define I2C_FOUND_TPA6130A2 (1u << 1)
#define I2C_FOUND_TOUCH    (1u << 2)

/* Checks the idle level of SDA/SCL (with pull-ups), tries a 9-clock bus
 * recovery if SDA is stuck low, then initialises the peripheral. Logs
 * everything. Returns false if a line is stuck low (bus unusable). */
bool i2c_bus_init(void);

/* True if `addr` ACKs within `tries` attempts (3 ms apart; some chips NACK
 * while asleep). */
bool i2c_bus_probe(uint8_t addr, int tries);

/* Scans 0x08..0x77, logs a table of expected chips (found / MISSING) plus any
 * unexpected address, with wiring hints. Returns the I2C_FOUND_* mask. */
uint32_t i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif
