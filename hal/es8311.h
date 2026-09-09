/*
 * hal/es8311.h — ES8311 driver (DAC + ADC)
 *
 * Initializes the ES8311 codec over I2C: DAC path (output) and ADC path (input).
 * Designed for the WeAct ES8311 Mono Audio Codec Module.
 *
 * ⚠ Critical constraint: call es8311_init() BEFORE mclk_init().
 *   The MCLK PWM generates EMI on the I2C lines.
 *   Call es8311_power_up() AFTER MCLK is stable.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bus parameters ───────────────────────────────────────────────────────── */
#define ES8311_I2C_ADDR   0x18   /* CE pin tied to GND on the WeAct board */
#define ES8311_I2C_SPEED  100000 /* 100 kHz — DO NOT use 400 kHz (MCLK EMI) */

/* ── Registers (excerpt from the ES8311 datasheet) ───────────────────────── */
#define ES8311_REG_RESET      0x00  /* reset + mode (slave/master) + CSM_ON */
#define ES8311_REG_CLK1       0x01  /* CLK_MGR1  : enable MCLK/BCLK/DAC/ANACLK */
#define ES8311_REG_CLK2       0x02  /* CLK_MGR2  : MCLK->Fs divider */
#define ES8311_REG_CLK3       0x03  /* CLK_MGR3  : ADC oversampling ratio */
#define ES8311_REG_CLK4       0x04  /* CLK_MGR4  : DAC oversampling ratio */
#define ES8311_REG_CLK5       0x05  /* CLK_MGR5  : ADC/DAC pre-divider */
#define ES8311_REG_CLK6       0x06  /* CLK_MGR6  : BCLK divider (slave -> ignored) */
#define ES8311_REG_CLK7       0x07  /* CLK_MGR7  : tri-state BCLK/LRCK */
#define ES8311_REG_DAC_FMT    0x09  /* DAC interface format */
#define ES8311_REG_ADC_FMT    0x0A  /* ADC interface format */
#define ES8311_REG_SYS1       0x0D  /* SYS_CTL1  : analog power */
#define ES8311_REG_SYS2       0x0E  /* SYS_CTL2  : PGA + ADC modulator enable */
#define ES8311_REG_PDN_DAC    0x12  /* power-down DAC + reference */
#define ES8311_REG_HPSW       0x13  /* HP switch (NS4150B on the WeAct board) */
#define ES8311_REG_ADC_ANA    0x14  /* ADC/MIC: DMIC_ON, LINSEL, PGAGAIN */
#define ES8311_REG_MIC_GAIN   0x16  /* mic PGA gain: bits[4:2] = 0..8 (0..+24dB) */
#define ES8311_REG_ADC_VOL    0x17  /* ADC digital volume */
#define ES8311_REG_ADC_CTRL   0x1C  /* ADC enable/mute */
#define ES8311_REG_ADC_LEVEL  0x21  /* ADC peak meter (read-only) */
#define ES8311_REG_DAC_CTL    0x31  /* DAC control (mute, inversion) */
#define ES8311_REG_DAC_VOL    0x32  /* DAC volume: 0x00=-95.5dB, 0xBF=0dB */
#define ES8311_REG_DAC_RMP    0x37  /* DACEQ_B0[15:8] (EQ coeff, leave at 0x08) */
#define ES8311_REG_CHIP_ID1   0xFD  /* chip ID byte 1 (expected: 0x83) */
#define ES8311_REG_CHIP_ID2   0xFE  /* chip ID byte 2 (expected: 0x11) */
#define ES8311_REG_CSM_FLAG   0xFC  /* CSM state: bits[6:4] = S0..S7 */

/* ── API ──────────────────────────────────────────────────────────────────── */

/*
 * es8311_init() — configures all DAC + ADC registers.
 * Call BEFORE mclk_init(). I2C must already be initialized.
 * Returns false if the chip is not detected.
 */
bool es8311_init(void);

/*
 * es8311_power_up() — enables the Chip State Machine (CSM_ON = REG0x00 bit7).
 * Call AFTER mclk_init(). Waits ~50 ms to reach S7 (fully powered).
 */
void es8311_power_up(void);

/*
 * es8311_set_dac_volume() — DAC volume in dB.
 * vol_db: -95.5..+32.0 dB. 0.0 dB = 0xBF (no attenuation).
 */
void es8311_set_dac_volume(float vol_db);

/*
 * es8311_set_mic_gain() — mic PGA gain.
 * gain: 0..8 -> 0..+24 dB in 3 dB steps.
 */
void es8311_set_mic_gain(uint8_t gain);

#ifdef __cplusplus
}
#endif
