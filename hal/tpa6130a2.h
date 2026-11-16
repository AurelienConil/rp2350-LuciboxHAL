/*
 * hal/tpa6130a2.h — TPA6130A2 stereo headphone amplifier driver (I2C 0x60)
 *
 * Datasheet SLOS488F. Shared bus (hal/i2c_bus.h). The chip only answers while
 * its SD pin is high; SD low = hardware shutdown, all registers lost.
 *
 * Registers (Table 3):
 *   REG1  HP_EN_L(7) HP_EN_R(6) MODE[1:0](5:4) -(3:2) THERMAL(1) SWS(0)   reset 0x00
 *   REG2  MUTE_L(7)  MUTE_R(6)  VOLUME[5:0]                                reset 0xC0
 *   REG3  -(7:2) HIZ_L(1) HIZ_R(0)                                         reset 0x00
 *   REG4  version[3:0], expected 0x02
 *   REG5..8 reserved for TI testing: NEVER written.
 * Bits marked "reserved" must not be written: this driver keeps shadow copies
 * and only ever writes the documented fields.
 *
 * Volume: 64-step NON-linear audio taper (Table 2), code 0 = -59.5 dB,
 * code 63 = +4.0 dB, 0x35 = +0.1 dB (about unity). Mute is separate (-100 dB).
 *
 * Power-up sequence done by tpa_init() (pop-free, per the de-pop block):
 *   1. read version, 2. volume set while still muted, 3. enable amps (still
 *   muted), 4. unmute. Startup time from shutdown is ~5 ms.
 *
 * Blocking I2C: init / main loop only, never from the audio IRQ.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TPA_I2C_ADDR      0x60
#define TPA_VERSION_EXPECTED 0x02

#define TPA_VOLUME_MIN    0
#define TPA_VOLUME_MAX    63
#define TPA_VOLUME_UNITY  0x35   /* +0.1 dB */

/* Default power-up volume code. Chain: ES8311 DAC (0 dB) -> TPA -> headphones,
 * which is very loud. 0x05 = -43.9 dB; override with -DTPA_VOLUME=0x.. */
#ifndef TPA_VOLUME
#define TPA_VOLUME        0x05
#endif

typedef enum {
    TPA_MODE_STEREO    = 0,  /* L/R independent (default) */
    TPA_MODE_DUAL_MONO = 1,  /* left input -> both outputs */
    TPA_MODE_BTL       = 2   /* left input, single load between HPLEFT/HPRIGHT */
} tpa_mode_t;

/* Probes the chip, checks the version, applies TPA_VOLUME, enables both
 * amplifiers in stereo mode and unmutes. Logs REG values. Returns false if the
 * chip does not answer (check SD pin high); every other call is then a no-op. */
bool tpa_init(void);

/* True once tpa_init() succeeded. */
bool tpa_present(void);

/* Volume code 0..63 (clamped). Mute state is preserved. */
bool tpa_set_volume(uint8_t code);
uint8_t tpa_get_volume(void);

/* Nominal gain in dB (Table 2) of a volume code. */
float tpa_volume_to_db(uint8_t code);

/* Highest code whose gain is <= `db` (clamped to 0..63). */
uint8_t tpa_db_to_volume(float db);

/* Sets the volume from a target gain in dB. */
bool tpa_set_volume_db(float db);

/* Mutes / unmutes both channels (volume code kept). */
bool tpa_mute(bool mute);
bool tpa_is_muted(void);

/* Software shutdown: charge pump + outputs off (~25 uA). Waking re-enables the
 * amplifiers with the stored volume / mute / mode (~5 ms before sound). */
bool tpa_shutdown(bool shutdown);

/* Output mode. Takes effect with the amplifiers enabled. */
bool tpa_set_mode(tpa_mode_t mode);

/* Hi-Z outputs (for sharing the jack with another driver). The chip requires
 * the amplifiers to be disabled: this disables them, then sets HiZ; clearing it
 * re-enables them. */
bool tpa_set_hiz(bool hiz);

/* Reads REG1: returns true if a thermal shutdown happened since the last call
 * (the THERMAL bit is clear-on-read). The chip restarts by itself once cool. */
bool tpa_thermal_fault(void);

/* Logs REG1..REG4 as seen on the chip (bring-up). */
void tpa_dump(void);

#ifdef __cplusplus
}
#endif
