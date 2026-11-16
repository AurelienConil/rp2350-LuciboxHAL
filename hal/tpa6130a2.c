/*
 * hal/tpa6130a2.c — TPA6130A2 headphone amplifier driver. See tpa6130a2.h.
 */

#include "tpa6130a2.h"
#include "i2c_bus.h"
#include <stdio.h>

#define REG1 0x01
#define REG2 0x02
#define REG3 0x03
#define REG4 0x04

/* REG1 */
#define R1_HP_EN_L   (1u << 7)
#define R1_HP_EN_R   (1u << 6)
#define R1_MODE_SHIFT 4
#define R1_SWS       (1u << 0)
/* REG2 */
#define R2_MUTE_L    (1u << 7)
#define R2_MUTE_R    (1u << 6)
#define R2_VOL_MASK  0x3Fu
/* REG3 */
#define R3_HIZ_L     (1u << 1)
#define R3_HIZ_R     (1u << 0)

/* Nominal gain (dB) for VOLUME[5:0] = 0..63 (datasheet Table 2). */
static const float VOLUME_DB[64] = {
    -59.5f,-53.5f,-50.0f,-47.5f,-45.5f,-43.9f,-41.4f,-39.5f,
    -36.5f,-35.3f,-33.3f,-31.7f,-30.4f,-28.6f,-27.1f,-26.3f,
    -24.7f,-23.7f,-22.5f,-21.7f,-20.5f,-19.6f,-18.8f,-17.8f,
    -17.0f,-16.2f,-15.2f,-14.5f,-13.7f,-13.0f,-12.3f,-11.6f,
    -10.9f,-10.3f, -9.7f, -9.0f, -8.5f, -7.8f, -7.2f, -6.7f,
     -6.1f, -5.6f, -5.1f, -4.5f, -4.1f, -3.5f, -3.1f, -2.6f,
     -2.1f, -1.7f, -1.2f, -0.8f, -0.3f,  0.1f,  0.5f,  0.9f,
      1.4f,  1.7f,  2.1f,  2.5f,  2.9f,  3.3f,  3.6f,  4.0f };

/* Shadow state: what we want the chip to hold (REG5..8 are never touched). */
static struct {
    bool       present;
    bool       shutdown;
    bool       hiz;
    bool       muted;
    uint8_t    volume;
    tpa_mode_t mode;
} s;

static bool reg_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    for (int i = 0; i < 3; i++) {                 /* retry: NACK right after wake-up */
        if (i2c_write_blocking(LUCIBOX_I2C, TPA_I2C_ADDR, b, 2, false) == 2) return true;
    }
    return false;
}

static bool reg_read(uint8_t reg, uint8_t *v) {
    for (int i = 0; i < 3; i++) {
        if (i2c_write_blocking(LUCIBOX_I2C, TPA_I2C_ADDR, &reg, 1, true) == 1 &&
            i2c_read_blocking(LUCIBOX_I2C, TPA_I2C_ADDR, v, 1, false) == 1) return true;
    }
    return false;
}

/* REG1 value from shadow state. Writing it also clears the THERMAL bit read
 * back, but never sets it: the chip owns that bit. */
static bool apply_reg1(void) {
    uint8_t v = (uint8_t)((unsigned)s.mode << R1_MODE_SHIFT);
    if (s.shutdown) v |= R1_SWS;                   /* SWS clears HP_EN on the chip */
    else if (!s.hiz) v |= R1_HP_EN_L | R1_HP_EN_R;
    return reg_write(REG1, v);
}

static bool apply_reg2(void) {
    uint8_t v = (uint8_t)(s.volume & R2_VOL_MASK);
    if (s.muted) v |= R2_MUTE_L | R2_MUTE_R;
    return reg_write(REG2, v);
}

static bool apply_reg3(void) {
    return reg_write(REG3, s.hiz ? (R3_HIZ_L | R3_HIZ_R) : 0);
}

bool tpa_init(void) {
    s.present = false;
    uint8_t ver;
    if (!reg_read(REG4, &ver)) {
        printf("[TPA] no answer at 0x%02X (SD pin must be high)\n", TPA_I2C_ADDR);
        return false;
    }
    ver &= 0x0F;
    printf("[TPA] version 0x%X%s\n", ver,
           ver == TPA_VERSION_EXPECTED ? "" : " (expected 0x2 — unexpected silicon?)");

    s.present  = true;
    s.shutdown = false;
    s.hiz      = false;
    s.mode     = TPA_MODE_STEREO;
    s.volume   = (uint8_t)(TPA_VOLUME & R2_VOL_MASK);
    s.muted    = true;                             /* stay muted while enabling */

    bool ok = apply_reg3() && apply_reg2() && apply_reg1();
    s.muted = false;                               /* then unmute: no pop */
    ok = ok && apply_reg2();

    tpa_dump();
    printf("[TPA] volume code 0x%02X = %.1f dB, stereo, %s\n", s.volume,
           (double)VOLUME_DB[s.volume], ok ? "enabled" : "WRITE FAILED");
    if (!ok) s.present = false;
    return ok;
}

bool tpa_present(void) { return s.present; }

bool tpa_set_volume(uint8_t code) {
    if (!s.present) return false;
    if (code > TPA_VOLUME_MAX) code = TPA_VOLUME_MAX;
    s.volume = code;
    return apply_reg2();
}

uint8_t tpa_get_volume(void) { return s.volume; }

float tpa_volume_to_db(uint8_t code) {
    return VOLUME_DB[code > TPA_VOLUME_MAX ? TPA_VOLUME_MAX : code];
}

uint8_t tpa_db_to_volume(float db) {
    uint8_t best = 0;
    for (uint8_t i = 0; i <= TPA_VOLUME_MAX; i++) {
        if (VOLUME_DB[i] <= db) best = i; else break;
    }
    return best;
}

bool tpa_set_volume_db(float db) { return tpa_set_volume(tpa_db_to_volume(db)); }

bool tpa_mute(bool mute) {
    if (!s.present) return false;
    s.muted = mute;
    return apply_reg2();
}

bool tpa_is_muted(void) { return s.muted; }

bool tpa_shutdown(bool shutdown) {
    if (!s.present) return false;
    s.shutdown = shutdown;
    /* Waking from SWS: the chip kept REG2/REG3 but REG1 needs a full rewrite. */
    return apply_reg1();
}

bool tpa_set_mode(tpa_mode_t mode) {
    if (!s.present) return false;
    s.mode = mode;
    return apply_reg1();
}

bool tpa_set_hiz(bool hiz) {
    if (!s.present) return false;
    if (hiz) {
        s.hiz = true;
        return apply_reg1() && apply_reg3();       /* amps off first, then HiZ */
    }
    s.hiz = false;
    return apply_reg3() && apply_reg1();           /* HiZ off first, then amps on */
}

bool tpa_thermal_fault(void) {
    uint8_t r1;
    if (!s.present || !reg_read(REG1, &r1)) return false;
    return (r1 & (1u << 1)) != 0;                  /* clear-on-read */
}

void tpa_dump(void) {
    uint8_t r[5] = {0};
    for (uint8_t i = 1; i <= 4; i++) {
        if (!reg_read(i, &r[i])) { printf("[TPA] read REG%u failed\n", i); return; }
    }
    printf("[TPA] REG1=0x%02X REG2=0x%02X REG3=0x%02X REG4=0x%02X\n", r[1], r[2], r[3], r[4]);
}
