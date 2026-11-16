/*
 * hal/es8311.c — ES8311 driver implementation (DAC + ADC)
 *
 * Registers validated on WeAct ES8311 + RP2350 (rp2350-BasicSine, June 2026).
 * Adds the ADC path (mic/line input) compared to the reference project.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "es8311.h"
#include "i2c_bus.h"

/* I2C instance used: the board's shared bus (hal/i2c_bus.h) */
#define ES8311_I2C  LUCIBOX_I2C

/* ── Low-level I2C helpers ────────────────────────────────────────────────── */

static void es_wr(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    int r = i2c_write_timeout_us(ES8311_I2C, ES8311_I2C_ADDR, buf, 2, false, 5000);
    if (r < 0)
        printf("  [ES8311] WRITE ERROR 0x%02X=0x%02X (%d)\n", reg, val, r);
}

static uint8_t es_rd(uint8_t reg) {
    uint8_t val = 0;
    i2c_write_timeout_us(ES8311_I2C, ES8311_I2C_ADDR, &reg, 1, true,  5000);
    i2c_read_timeout_us (ES8311_I2C, ES8311_I2C_ADDR, &val, 1, false, 5000);
    return val;
}

/* ── Register initialization ─────────────────────────────────────────────── */

bool es8311_init(void) {
    /* Check chip presence */
    uint8_t id1 = es_rd(ES8311_REG_CHIP_ID1);
    uint8_t id2 = es_rd(ES8311_REG_CHIP_ID2);
    printf("[ES8311] chip ID: 0x%02X 0x%02X (expected 0x83 0x11)\n", id1, id2);
    if (id1 != 0x83 || id2 != 0x11) {
        printf("[ES8311] ERROR: chip not detected. Check I2C wiring.\n");
        return false;
    }

    /* REG 0x00: slave mode, CSM_ON=0 (set to 1 after MCLK in power_up) */
    es_wr(ES8311_REG_RESET, 0x00);

    /* ── Clocks ──────────────────────────────────────────────────────────────
     *
     * REG 0x01 CLK1 (User Guide §8.3/8.5) : bit5 MCLK_ON, bit4 BCLK_ON,
     *   bit3 CLKADC_ON, bit2 CLKDAC_ON. Bits 1:0 non documentés dans le guide
     *   (bit 0 = ANACLKDAC_ON d'après le driver Espressif : sans lui DAC muet).
     *   0x35 = DAC seul (CLKADC_ON=0 -> ADC non cadencé -> sort des zéros exacts).
     *   0x3F = tout activé (valeur du driver de référence).
     *
     * REG 0x02 CLK2: MCLK/Fs ratio = 256 -> 0x04
     *   Our MCLK = 12.5 MHz, Fs = 48 kHz -> ratio 260.4 ≈ 256 (acceptable)
     *
     * REG 0x03 CLK3: ADC OSR = 0x10 (minimum valid value = 0x10, 64×Fs)
     *   ⚠ Values 0x00..0x0F = silent ADC (same pitfall as REG 0x04 for the DAC)
     *
     * REG 0x04 CLK4: DAC OSR = 0x10 (minimum valid value)
     *   ⚠ Values 0x00..0x0F = total silence — confirmed bug from rp2350-BasicSine
     */
    es_wr(ES8311_REG_CLK1, 0x3F);  /* 0x35 + CLKADC_ON (bit 3) : sans lui l'ADC sort 0 */
    es_wr(ES8311_REG_CLK2, 0x04);
    es_wr(ES8311_REG_CLK3, 0x10);  /* ADC OSR */
    es_wr(ES8311_REG_CLK4, 0x10);  /* DAC OSR */
    es_wr(ES8311_REG_CLK5, 0x00);  /* no ADC/DAC pre-divider */
    es_wr(ES8311_REG_CLK6, 0x03);  /* BCLK divider (ignored in slave mode) */
    es_wr(ES8311_REG_CLK7, 0x00);  /* BCLK/LRCK outputs active */

    /* ── Audio format ────────────────────────────────────────────────────────
     *
     * REG 0x09 DAC_FMT and REG 0x0A ADC_FMT: 0x0C = I2S Philips, 16 bits/channel
     *   bits[1:0] FORMAT = 00 -> I2S Philips
     *   bits[4:2] WLEN   = 011 (= 3) -> 16 bits
     *   0x0C = 0000 1100 = (3 << 2) | 0
     */
    es_wr(ES8311_REG_DAC_FMT, 0x0C);
    es_wr(ES8311_REG_ADC_FMT, 0x0C);

    /* ── Analog power ────────────────────────────────────────────────────────
     *
     * REG 0x0D SYS1: 0x06 = VREF enabled (INVERTED logic: PDN_VREF=1 = enable)
     *   ⚠ Pitfall: 0x0A disables VREF -> total silence.
     *   bit 2 PDN_VREF = 1 -> VREF enabled (1 = enable, not disable!)
     *   bits 1:0 VMIDSEL = 10 -> normal vmid (AVDD/2)
     *
     * REG 0x0E SYS2: 0x00 = PGA enabled + ADC modulator enabled
     *   Required for the ADC to work (PGA = input pre-amp)
     */
    es_wr(ES8311_REG_SYS1, 0x06);
    es_wr(ES8311_REG_SYS2, 0x02);   /* ref Espressif : 0x02 (était 0x00) */
    es_wr(0x10, 0x1F);              /* ref Espressif : bias analogiques (SYSTEM_REG10) */
    es_wr(0x11, 0x7F);              /* ref Espressif : (SYSTEM_REG11) */

    /* ── DAC ─────────────────────────────────────────────────────────────────
     *
     * REG 0x12 PDN_DAC: 0x01 = DAC active + audio reference on
     *   bit 1 PDN_DAC = 0 (active, inverted logic)
     *   bit 0 ENREFR  = 1 (reference enabled)
     *
     * REG 0x13 HPSW: 0x10 = HP switch active -> NS4150B amp enabled
     *   Without this bit, no output on the WeAct module's speaker.
     *
     * REG 0x31 DAC_CTL: 0x00 = no mute
     *
     * REG 0x32 DAC_VOL: 0xBF = 0 dB (no attenuation)
     *   Scale: 0x00 = -95.5 dB, 0xBF = 0 dB, 0xFF = +32 dB
     */
    es_wr(ES8311_REG_PDN_DAC, 0x01);
    es_wr(ES8311_REG_HPSW,    0x10);
    es_wr(ES8311_REG_DAC_CTL, 0x00);
    es_wr(ES8311_REG_DAC_VOL, 0xBF);
    es_wr(ES8311_REG_DAC_RMP, 0x08);

    /* ── ADC (input path) ────────────────────────────────────────────────────
     *
     * REG 0x14 ADC_ANA: 0x10 = line input (LINE input) enabled
     *   bit 6 DMIC_ON = 0   -> digital mic disabled
     *   bit 4 LINSEL  = 1   -> MIC1P/MIC1N line input enabled
     *   bits 3:0 PGAGAIN = 0 -> 0 dB analog PGA gain (separate from MIC_GAIN)
     *
     * REG 0x16 MIC_GAIN: bits[4:2] = mic PGA gain
     *   0x08 = 0000 1000 -> bits[4:2] = 010 = +6 dB (reasonable default)
     *   Range: 0=0dB, 1=+3dB, 2=+6dB, ..., 8=+24dB (3 dB steps)
     *
     * REG 0x1C ADC_CTRL: 0x00 = ADC enabled, no mute
     */
    /* Micro analogique MIC1P/MIC1N (config du driver de référence Espressif) :
     *   0x14 = 0x10 | ES8311_PGA_GAIN_INIT : LINSEL=1 (MIC1P/N) + PGA (0 dB par défaut,
     *         source ligne/SmartRig ; max 10 = +30 dB pour un micro nu)
     *   0x16 = 0x04 : ADC_SCALE=4 = +24 dB (défaut du guide §10.6 ; bit 3 = ADC_RAMCLR, laissé à 0)
     *   0x17 = 0xBF : ADC_VOL 0 dB — ⚠ défaut 0x00 = -95.5 dB = ADC muet !
     *   0x1C = 0x6A : ADC_EQ_BYPASS + filtre passe-haut DC */
    es_wr(ES8311_REG_ADC_ANA,  0x10 | (ES8311_PGA_GAIN_INIT & 0x0F));
    es_wr(ES8311_REG_MIC_GAIN, ES8311_ADC_SCALE_INIT & 0x07);
    es_wr(ES8311_REG_ADC_VOL,  0xBF);
    es_wr(0x1B, 0x0A);              /* ADC HPF (ref Espressif) */
    es_wr(ES8311_REG_ADC_CTRL, 0x6A);

    /* Read-back for diagnostics */
    printf("[ES8311] REG0x01=0x%02X (expected 0x3F)\n", es_rd(ES8311_REG_CLK1));
    printf("[ES8311] REG0x0D=0x%02X (expected 0x06)\n", es_rd(ES8311_REG_SYS1));
    printf("[ES8311] REG0x32=0x%02X (DAC vol)\n",        es_rd(ES8311_REG_DAC_VOL));
    printf("[ES8311] REG0x17=0x%02X (ADC vol, expected 0xBF)\n", es_rd(ES8311_REG_ADC_VOL));
    printf("[ES8311] REG0x1C=0x%02X (ADC ctrl)\n",       es_rd(ES8311_REG_ADC_CTRL));
    printf("[ES8311] Registers ok (CSM_ON not active yet)\n");

    return true;
}

/* ── Power-up (CSM_ON) ───────────────────────────────────────────────────── */

void es8311_power_up(void) {
    /* REG 0x00 bit7 = CSM_ON: enables the Chip State Machine.
     * The CSM transitions S0->S7 (fully powered) only if MCLK is stable.
     * ⚠ Call AFTER mclk_init() and enabling PIO/DMA. */
    es_wr(ES8311_REG_RESET, 0x80);
    sleep_ms(50);  /* ~30 ms typical to reach S7 */

    uint8_t flag = es_rd(ES8311_REG_CSM_FLAG);
    uint8_t csm  = (flag >> 4) & 0x07;
    printf("[ES8311] CSM = S%d (0x%02X) %s\n",
           csm, flag, csm == 7 ? "-> OK" : "-> ERROR: MCLK absent or wrong ratio?");

    /* DEBUG ADC : dump des registres utiles après power-up */
    static const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x0A,0x0D,0x0E,0x10,0x11,0x14,0x15,0x16,0x17,0x1B,0x1C,0x44};
    printf("[ES8311] dump:");
    for (unsigned i = 0; i < sizeof regs; i++)
        printf(" %02X=%02X", regs[i], es_rd(regs[i]));
    printf("\n");
}

/* ── Runtime controls ────────────────────────────────────────────────────── */

void es8311_set_dac_volume(float vol_db) {
    /* dB -> register conversion: step = 0.5 dB, 0 dB = 0xBF = 191 */
    int reg = (int)(191.0f + vol_db * 2.0f);
    if (reg < 0)   reg = 0;
    if (reg > 255) reg = 255;
    es_wr(ES8311_REG_DAC_VOL, (uint8_t)reg);
}

void es8311_set_mic_gain(uint8_t gain) {
    if (gain > 10) gain = 10;
    uint8_t cur = es_rd(ES8311_REG_ADC_ANA);
    cur = (cur & 0xF0u) | gain;                     /* PGAGAIN = bits[3:0] */
    es_wr(ES8311_REG_ADC_ANA, cur);
}
