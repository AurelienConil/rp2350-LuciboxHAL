/*
 * hal/memtest.c — Validation flash QSPI + PSRAM (voir memtest.h)
 *
 * Contraintes :
 *  - Toute opération qui touche la flash (JEDEC, erase, program) ou le QMI en
 *    mode direct coupe le XIP : IRQ désactivées, code exécuté depuis la SRAM
 *    (__no_inline_not_in_flash_func), jamais d'appel vers du code en flash.
 *  - Les tests PSRAM passent par l'alias SANS cache (0x15000000) pour que
 *    chaque accès atteigne réellement la puce.
 */

#include "hal/memtest.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#define MB (1024u * 1024u)

extern char __flash_binary_end;   /* fin de l'image firmware dans la flash */

/* ── Étape courante, conservée à travers un reset watchdog ──────────────────
 * Le log USB devient peu fiable autour des accès PSRAM : on note l'étape dans
 * les registres scratch du watchdog (qui survivent à son reset). Si le test se
 * fige, le watchdog redémarre la carte et le boot suivant annonce l'étape. */
#define MT_MAGIC 0x4D454D54u   /* 'MEMT' */
static inline void mt_stage(uint32_t s) {
    watchdog_hw->scratch[0] = MT_MAGIC;
    watchdog_hw->scratch[1] = s;
}
static inline void mt_feed(void) { watchdog_update(); }

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilitaires
 * ═══════════════════════════════════════════════════════════════════════ */

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* Débit en dixièmes de Mo/s (entiers : pas de flottant dans les logs). */
static uint32_t mbps_x10(uint32_t bytes, uint64_t us) {
    if (us == 0) us = 1;
    return (uint32_t)(((uint64_t)bytes * 10u * 1000000u) / (us * (uint64_t)MB));
}

/* Compteur d'erreurs avec mémoire des premières + masque des bits touchés. */
typedef struct {
    uint32_t count;
    uint32_t bitmask;     /* OR des (attendu ^ lu) : bits de donnée fautifs */
    uint32_t shown;
} errlog_t;

static void err_record(errlog_t *e, const char *tag, uint32_t addr,
                       uint32_t expected, uint32_t got) {
    e->count++;
    e->bitmask |= expected ^ got;
    if (e->shown < 8) {
        e->shown++;
        printf("    %s ERREUR @0x%08lX : attendu 0x%08lX, lu 0x%08lX (xor 0x%08lX)\n",
               tag, (unsigned long)addr, (unsigned long)expected,
               (unsigned long)got, (unsigned long)(expected ^ got));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLASH
 * ═══════════════════════════════════════════════════════════════════════ */

/* flash_do_cmd() coupe le XIP : IRQ off le temps de la commande. */
static void flash_cmd(const uint8_t *tx, uint8_t *rx, size_t n) {
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, n);
    restore_interrupts(irq);
}

static const char *flash_vendor(uint8_t mf) {
    switch (mf) {
        case 0xEF: return "Winbond";
        case 0xC2: return "Macronix";
        case 0xC8: return "GigaDevice";
        case 0x20: return "Micron/XMC";
        case 0x9D: return "ISSI";
        case 0x1F: return "Adesto/Atmel";
        case 0x0B: return "XTX";
        case 0x85: return "Puya";
        case 0x68: return "Boya";
        case 0xA1: return "Fudan";
        case 0x25: return "Zbit";
        default:   return "inconnu";
    }
}

/* Lit n octets SFDP à l'adresse addr (cmd 0x5A, 3 octets d'adresse, 1 dummy). */
static void flash_sfdp_read(uint32_t addr, uint8_t *out, size_t n) {
    uint8_t tx[5 + 16] = {0}, rx[5 + 16];
    tx[0] = 0x5A;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)addr;
    flash_cmd(tx, rx, 5 + n);
    memcpy(out, rx + 5, n);
}

/* Densité SFDP en octets (0 si SFDP absent). */
static uint32_t flash_sfdp_bytes(void) {
    uint8_t h[16];
    flash_sfdp_read(0, h, 16);
    if (memcmp(h, "SFDP", 4) != 0) {
        printf("[FLASH] SFDP : absent (signature %02X %02X %02X %02X)\n",
               h[0], h[1], h[2], h[3]);
        return 0;
    }
    uint32_t ptr = h[12] | (h[13] << 8) | (h[14] << 16);   /* BFPT */
    uint8_t d[4];
    flash_sfdp_read(ptr + 4, d, 4);                          /* DWORD 2 */
    uint32_t v = d[0] | (d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    uint64_t bits = (v & 0x80000000u) ? (1ull << (v & 0x7FFFFFFFu))
                                      : ((uint64_t)v + 1u);
    uint32_t bytes = (uint32_t)(bits / 8u);
    printf("[FLASH] SFDP : rev %u.%u, densité %lu Mbit = %lu Mo\n",
           h[5], h[4], (unsigned long)(bits >> 20), (unsigned long)(bytes / MB));
    return bytes;
}

static void flash_fill_pattern(uint8_t *buf, uint32_t seed) {
    uint32_t s = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i += 4) {
        uint32_t w = xorshift32(&s);
        memcpy(buf + i, &w, 4);
    }
}

static uint8_t s_sector_buf[FLASH_SECTOR_SIZE];

/* Test destructif : un secteur de test par tranche de capacité. */
static uint32_t flash_write_test(uint32_t limit_bytes, uint32_t binary_end_off) {
    uint32_t errors = 0;
    uint32_t offs[5];
    int      n = 0;

    for (uint32_t size = 1 * MB; size <= limit_bytes && size <= 16 * MB; size <<= 1) {
        uint32_t off = size - FLASH_SECTOR_SIZE;
        if (off < binary_end_off) {
            printf("[FLASH] secteur de test 0x%06lX ignoré : sous la fin du firmware (0x%06lX)\n",
                   (unsigned long)off, (unsigned long)binary_end_off);
            continue;
        }
        offs[n++] = off;
    }
    if (n == 0) {
        printf("[FLASH] aucun secteur de test disponible\n");
        return 0;
    }

    printf("[FLASH] test écriture/effacement sur %d secteur(s) de fin de tranche :\n", n);
    uint64_t erase_us_max = 0, prog_us_max = 0;

    /* 1) erase + program, secteur par secteur (ordre croissant). */
    for (int i = 0; i < n; i++) {
        flash_fill_pattern(s_sector_buf, offs[i]);
        uint64_t t0 = time_us_64();
        uint32_t irq = save_and_disable_interrupts();
        flash_range_erase(offs[i], FLASH_SECTOR_SIZE);
        restore_interrupts(irq);
        uint64_t t1 = time_us_64();

        /* Effacé = tout à 0xFF (lecture sans cache). */
        const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs[i]);
        uint32_t nonff = 0;
        for (uint32_t k = 0; k < FLASH_SECTOR_SIZE; k++) if (p[k] != 0xFF) nonff++;
        if (nonff) {
            printf("    0x%06lX : %lu octets non effacés après erase\n",
                   (unsigned long)offs[i], (unsigned long)nonff);
            errors++;
        }

        uint64_t t2 = time_us_64();
        irq = save_and_disable_interrupts();
        flash_range_program(offs[i], s_sector_buf, FLASH_SECTOR_SIZE);
        restore_interrupts(irq);
        uint64_t t3 = time_us_64();

        if (t1 - t0 > erase_us_max) erase_us_max = t1 - t0;
        if (t3 - t2 > prog_us_max)  prog_us_max  = t3 - t2;
    }

    /* 2) Relecture de TOUS les secteurs après toutes les écritures : un secteur
     *    qui se replie sur un autre (flash plus petite) est détecté ici. */
    for (int i = 0; i < n; i++) {
        flash_fill_pattern(s_sector_buf, offs[i]);
        const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs[i]);
        uint32_t bad = 0, first = 0;
        for (uint32_t k = 0; k < FLASH_SECTOR_SIZE; k++) {
            if (p[k] != s_sector_buf[k]) { if (!bad) first = k; bad++; }
        }
        printf("    0x%06lX (fin de tranche %2lu Mo) : %s",
               (unsigned long)offs[i], (unsigned long)((offs[i] + FLASH_SECTOR_SIZE) / MB),
               bad ? "ERREUR" : "ok");
        if (bad) {
            printf(" — %lu octets faux, 1er à +%lu (attendu %02X, lu %02X)"
                   " → repliement d'adresse / capacité < %lu Mo ?",
                   (unsigned long)bad, (unsigned long)first,
                   s_sector_buf[first], p[first],
                   (unsigned long)((offs[i] + FLASH_SECTOR_SIZE) / MB));
            errors++;
        }
        printf("\n");
    }

    printf("[FLASH] temps max : effacement 4 Ko = %lu ms, programmation 4 Ko = %lu ms\n",
           (unsigned long)(erase_us_max / 1000u), (unsigned long)(prog_us_max / 1000u));

    /* 3) Nettoyage : on laisse la flash de test vierge. */
    for (int i = 0; i < n; i++) {
        uint32_t irq = save_and_disable_interrupts();
        flash_range_erase(offs[i], FLASH_SECTOR_SIZE);
        restore_interrupts(irq);
        const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs[i]);
        for (uint32_t k = 0; k < FLASH_SECTOR_SIZE; k++) {
            if (p[k] != 0xFF) { errors++; printf("    nettoyage 0x%06lX incomplet\n", (unsigned long)offs[i]); break; }
        }
    }
    return errors;
}

static void flash_run(memtest_report_t *rep) {
    printf("\n──────────────── [FLASH] ────────────────\n");
    printf("[FLASH] configuré : PICO_FLASH_SIZE_BYTES = %lu Mo, SPI clkdiv = %d (clk_sys/%d)\n",
           (unsigned long)(PICO_FLASH_SIZE_BYTES / MB), PICO_FLASH_SPI_CLKDIV, PICO_FLASH_SPI_CLKDIV);

    /* ID JEDEC */
    uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    flash_cmd(tx, rx, 4);
    memcpy(rep->flash_jedec, rx + 1, 3);
    printf("[FLASH] JEDEC : %02X %02X %02X — fabricant %s",
           rx[1], rx[2], rx[3], flash_vendor(rx[1]));
    uint32_t jedec_bytes = 0;
    if (rx[3] >= 0x10 && rx[3] <= 0x1F) jedec_bytes = 1u << rx[3];
    if (jedec_bytes) printf(", capacité %lu Mo (octet 0x%02X)\n", (unsigned long)(jedec_bytes / MB), rx[3]);
    else             printf(", capacité non décodable (octet 0x%02X)\n", rx[3]);
    if ((rx[1] == 0x00 || rx[1] == 0xFF)) {
        printf("[FLASH] ID invalide (tout à 0 ou 1) : la flash ne répond pas ?!\n");
    }

    /* ID unique */
    uint8_t uid[8];
    flash_get_unique_id(uid);
    printf("[FLASH] ID unique : %02X%02X%02X%02X%02X%02X%02X%02X\n",
           uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);

    /* SFDP */
    uint32_t sfdp_bytes = flash_sfdp_bytes();

    uint32_t bytes = sfdp_bytes ? sfdp_bytes : jedec_bytes;
    if (sfdp_bytes && jedec_bytes && sfdp_bytes != jedec_bytes)
        printf("[FLASH] ⚠ JEDEC (%lu Mo) ≠ SFDP (%lu Mo)\n",
               (unsigned long)(jedec_bytes / MB), (unsigned long)(sfdp_bytes / MB));
    rep->flash_bytes = bytes;
    if (bytes && bytes != PICO_FLASH_SIZE_BYTES)
        printf("[FLASH] ⚠ capacité réelle %lu Mo ≠ PICO_FLASH_SIZE_BYTES %lu Mo (boards/lucibox.h)\n",
               (unsigned long)(bytes / MB), (unsigned long)(PICO_FLASH_SIZE_BYTES / MB));

    /* Sondage d'alias non destructif : le contenu à l'offset S reproduit-il
     * celui de l'offset 0 (début du firmware) ? */
    const uint8_t *base = (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    bool start_blank = true;
    for (int i = 0; i < 256; i++) if (base[i] != 0xFF) { start_blank = false; break; }
    uint32_t alias_at = 0;
    if (!start_blank) {
        for (uint32_t s = 1 * MB; s < 16 * MB; s <<= 1) {
            if (memcmp(base, base + s, 256) == 0) { alias_at = s; break; }
        }
    }
    if (alias_at)
        printf("[FLASH] ⚠ alias détecté : le contenu à +%lu Mo reproduit celui de l'offset 0 → capacité réelle %lu Mo\n",
               (unsigned long)(alias_at / MB), (unsigned long)(alias_at / MB));
    else
        printf("[FLASH] sondage d'alias : aucun repliement jusqu'à 16 Mo (lecture)\n");

    /* Débit de lecture XIP (zone firmware, 256 Ko, avec cache puis sans). */
    {
        const volatile uint32_t *c  = (const volatile uint32_t *)XIP_BASE;
        const volatile uint32_t *nc = (const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
        const uint32_t words = (256u * 1024u) / 4u;
        uint32_t acc = 0;
        uint64_t t0 = time_us_64();
        for (uint32_t i = 0; i < words; i++) acc += nc[i];
        uint64_t t1 = time_us_64();
        for (uint32_t i = 0; i < words; i++) acc += c[i];
        uint64_t t2 = time_us_64();
        uint32_t a = mbps_x10(256u * 1024u, t1 - t0), b = mbps_x10(256u * 1024u, t2 - t1);
        printf("[FLASH] lecture XIP 256 Ko : sans cache %lu.%lu Mo/s, avec cache %lu.%lu Mo/s (somme 0x%08lX)\n",
               (unsigned long)(a / 10), (unsigned long)(a % 10),
               (unsigned long)(b / 10), (unsigned long)(b % 10), (unsigned long)acc);
    }

    /* Test écriture/effacement */
    uint32_t errors = 0;
    if (alias_at) errors++;
#if MEMTEST_FLASH_WRITE
    {
        uint32_t limit = bytes ? bytes : PICO_FLASH_SIZE_BYTES;
        if (limit > 16 * MB) limit = 16 * MB;
        uint32_t bin_end = ((uint32_t)&__flash_binary_end - XIP_BASE + FLASH_SECTOR_SIZE - 1)
                           & ~(FLASH_SECTOR_SIZE - 1);
        printf("[FLASH] firmware : %lu Ko (fin 0x%06lX)\n",
               (unsigned long)(bin_end / 1024u), (unsigned long)bin_end);
        errors += flash_write_test(limit, bin_end);
    }
#else
    printf("[FLASH] test écriture désactivé (MEMTEST_FLASH_WRITE=0)\n");
#endif

    rep->flash_errors = errors;
    rep->flash_ok = (errors == 0) && (rx[1] != 0x00 && rx[1] != 0xFF);
    printf("[FLASH] RÉSULTAT : %s (%lu erreur(s))\n",
           rep->flash_ok ? "OK" : "ÉCHEC", (unsigned long)errors);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PSRAM
 * ═══════════════════════════════════════════════════════════════════════ */

#define PSRAM_CACHED    ((volatile uint32_t *)0x11000000u)
#define PSRAM_UNCACHED  ((volatile uint32_t *)0x15000000u)   /* M1, sans cache */
#define PSRAM_WINDOW    (16u * MB)                           /* fenêtre XIP d'un CS */

#define PSRAM_ID_LEN 12   /* cmd + 3 adresse + MF + KGD + 6 EID */

/* Lit l'ID (cmd 0x9F, SPI) après avoir quitté un éventuel mode QPI. Retourne
 * les 12 octets reçus (MF = id[4], KGD = id[5], EID = id[6..11]). */
static void __no_inline_not_in_flash_func(psram_read_id)(uint8_t id[PSRAM_ID_LEN]) {
    uint32_t irq = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    /* Sortie du mode QPI (0xF5 en quad) au cas où la PSRAM y serait déjà
     * (reset du RP2350 sans coupure d'alimentation). */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | 0xF5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    /* Reset logiciel (datasheet §14) : Reset-Enable 0x66 puis Reset 0x99, en SPI,
     * CS relevé entre les deux. Remet la puce en SPI, état requis pour lire l'ID. */
    for (int c = 0; c < 2; c++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = (c == 0) ? 0x66u : 0x99u;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
        (void)qmi_hw->direct_rx;
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int i = 0; i < PSRAM_ID_LEN; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9Fu : 0xFFu;
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
        id[i] = (uint8_t)qmi_hw->direct_rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    restore_interrupts(irq);
}

/* Passe la PSRAM en quad et configure la fenêtre mémoire M1 (lecture 0xEB,
 * écriture 0x38) — timings de l'APS6404L, repris pour les 16 Mo QSPI. */
static void __no_inline_not_in_flash_func(psram_enable_xip)(uint32_t timing) {
    uint32_t irq = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    /* 0x35 : entrée en mode quad (envoyée en SPI simple). */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    qmi_hw->m[1].timing = timing;

    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M1_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8   << QMI_M1_RFMT_PREFIX_LEN_LSB)   |
        (QMI_M1_RFMT_DUMMY_LEN_VALUE_24   << QMI_M1_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xEBu << QMI_M1_RCMD_PREFIX_LSB;

    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_WFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M1_WFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_WFMT_DATA_WIDTH_LSB)   |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8   << QMI_M1_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38u << QMI_M1_WCMD_PREFIX_LSB;

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    restore_interrupts(irq);
}

/* Calcule le registre M1_TIMING pour clk_sys et la fréquence max demandée. */
static uint32_t psram_compute_timing(uint32_t sys_hz, uint32_t max_hz,
                                     uint32_t *divisor_out, uint32_t *rxdelay_out) {
    uint32_t divisor = (sys_hz + max_hz - 1) / max_hz;
    if (divisor < 2 && sys_hz > 100000000u) divisor = 2;
    if (divisor < 1) divisor = 1;
    if (divisor > 255) divisor = 255;
    uint32_t rxdelay = divisor;
    if (sys_hz / divisor > 100000000u) rxdelay += 1;
    if (rxdelay > 7) rxdelay = 7;

    /* tCEM (CS bas max) ≈ 8 µs : on vise 6 µs, en unités de 64 cycles clk_sys. */
    uint32_t max_select = (uint32_t)(((uint64_t)6000u * sys_hz / 1000000000u) / 64u);
    if (max_select < 1)  max_select = 1;
    if (max_select > 63) max_select = 63;
    /* tCPH (CS haut min) ≈ 18 ns, plus la demi-période de l'horloge PSRAM. */
    uint32_t min_deselect = (uint32_t)(((uint64_t)18u * sys_hz + 999999999u) / 1000000000u)
                            + (divisor + 1) / 2;
    if (min_deselect > 31) min_deselect = 31;

    *divisor_out = divisor;
    *rxdelay_out = rxdelay;
    return (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
           (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
           (max_select   << QMI_M1_TIMING_MAX_SELECT_LSB) |
           (min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
           (rxdelay      << QMI_M1_TIMING_RXDELAY_LSB) |
           (divisor      << QMI_M1_TIMING_CLKDIV_LSB);
}

static bool psram_id_valid(const uint8_t *id) {
    uint8_t mf = id[4], kgd = id[5];
    if (mf == 0x00 && kgd == 0x00) return false;
    if (mf == 0xFF && kgd == 0xFF) return false;
    return true;
}

/* Détection de taille par repliement d'adresse : un marqueur par puissance de
 * 2, puis l'offset 0. Renvoie la taille en octets (≤ fenêtre 16 Mo), et le
 * numéro de bit du premier repliement (0 si aucun). */
static uint32_t psram_detect_size(int *alias_bit) {
    volatile uint32_t *m = PSRAM_UNCACHED;
    *alias_bit = 0;
    for (int k = 2; k < 24; k++) {
        mt_stage(300u + (uint32_t)k);          /* 302..323 : écriture du marqueur k */
        m[(1u << k) / 4u] = 0xC0DE0000u + (uint32_t)k * 0x01010101u;
    }
    mt_stage(330);
    m[0] = 0x0FF5E700u;
    for (int k = 2; k < 24; k++) {
        mt_stage(400u + (uint32_t)k);          /* 402..423 : lecture du marqueur k */
        if (m[(1u << k) / 4u] != 0xC0DE0000u + (uint32_t)k * 0x01010101u) {
            *alias_bit = k;
            return 1u << k;
        }
    }
    return PSRAM_WINDOW;   /* aucun repliement jusqu'à 8 Mo → ≥ 16 Mo */
}

static uint32_t psram_pattern(int pass, uint32_t i, uint32_t *rng) {
    switch (pass) {
        case 0:  return i * 0x9E3779B1u;          /* unique par mot : révèle les alias */
        case 1:  return ~(i * 0x9E3779B1u);       /* inverse : bascule tous les bits */
        default: return xorshift32(rng);          /* pseudo-aléatoire */
    }
}

static void psram_report_mbps(const char *what, uint32_t bytes, uint64_t us) {
    uint32_t v = mbps_x10(bytes, us);
    printf("%s %lu.%lu Mo/s", what, (unsigned long)(v / 10), (unsigned long)(v % 10));
}

static void psram_run(memtest_report_t *rep) {
    printf("\n──────────────── [PSRAM] ────────────────\n");
    rep->psram_present = false;
    rep->psram_ok      = false;
    rep->psram_cs_pin  = -1;
    rep->psram_bytes   = 0;
    rep->psram_errors  = 0;

    /* 1) Quelle broche porte CS1 ? (lecture d'ID sur chaque candidate) */
    static const int candidates[] = {0, 8};
    int   cand_n = (int)(sizeof candidates / sizeof candidates[0]);
    int   fixed  = MEMTEST_PSRAM_CS_PIN;
    uint8_t id[PSRAM_ID_LEN] = {0};
    int   pin = -1;

    for (int c = 0; c < (fixed >= 0 ? 1 : cand_n); c++) {
        int p = (fixed >= 0) ? fixed : candidates[c];
        gpio_set_function((uint)p, GPIO_FUNC_XIP_CS1);
        mt_stage(100);
        psram_read_id(id);
        mt_stage(110);
        printf("[PSRAM] GPIO %2d : id = %02X %02X | %02X %02X %02X %02X %02X %02X (MF KGD | EID)\n",
               p, id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11]);
        if (psram_id_valid(id)) { pin = p; break; }
        gpio_deinit((uint)p);   /* pas de PSRAM ici : on libère la broche */
    }
    if (pin < 0) {
        printf("[PSRAM] aucune réponse (ID tout à 0x00/0xFF) sur GPIO %s\n",
               fixed >= 0 ? "imposé" : "0 et 8");
        printf("[PSRAM]   → vérifier : alimentation PSRAM, CS reliée à GPIO 0 ou 8 (XIP_CS1),\n"
               "[PSRAM]     SIO0..3 sur QSPI_SD0..3, CLK sur QSPI_SCLK, soudures.\n");
        printf("[PSRAM] RÉSULTAT : ABSENTE / MUETTE\n");
        return;
    }
    rep->psram_present = true;
    rep->psram_cs_pin  = pin;

    printf("[PSRAM] détectée : CS1 = GPIO %d, MF=0x%02X%s, KGD=0x%02X%s\n",
           pin, id[4], id[4] == 0x0D ? " (AP Memory)" : "", id[5],
           id[5] == 0x5D ? " (good die)" : id[5] == 0x55 ? " (die à problème)" : "");
    if (id[4] != 0x0D || id[5] != 0x5D)
        printf("[PSRAM] ⚠ ID ≠ APS6404 (0D 5D) : séquence APS6404 appliquée telle quelle "
               "(0x35, 0xEB, 0x38). Si les tests échouent, vérifier la datasheet de la puce "
               "(une PSRAM OPI/xSPI 1,8 V ne marche pas en QSPI).\n");

    /* 2) Init QMI */
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t divisor, rxdelay;
    uint32_t timing = psram_compute_timing(sys_hz, MEMTEST_PSRAM_MAX_HZ, &divisor, &rxdelay);
    printf("[PSRAM] clk_sys %lu MHz, diviseur %lu → SCLK %lu MHz, rxdelay %lu, page 1024 octets\n",
           (unsigned long)(sys_hz / 1000000u), (unsigned long)divisor,
           (unsigned long)(sys_hz / divisor / 1000000u), (unsigned long)rxdelay);
    printf("[PSRAM] timing = 0x%08lX, activation du mode quad + XIP M1...\n", (unsigned long)timing);
    sleep_ms(50);   /* laisse partir le log USB avant les accès risqués */
    mt_stage(120);
    psram_enable_xip(timing);
    mt_stage(130);
    printf("[PSRAM] XIP M1 configuré (qmi m1: timing=0x%08lX rfmt=0x%08lX wfmt=0x%08lX ctrl=0x%08lX)\n",
           (unsigned long)qmi_hw->m[1].timing, (unsigned long)qmi_hw->m[1].rfmt,
           (unsigned long)qmi_hw->m[1].wfmt, (unsigned long)xip_ctrl_hw->ctrl);
    sleep_ms(50);
    {
        /* Jalons : si le log s'arrête sur l'un d'eux, c'est cet accès qui bloque. */
        mt_stage(140);
        uint32_t r0 = PSRAM_UNCACHED[0];
        mt_stage(150);
        PSRAM_UNCACHED[0] = 0x12345678u;
        mt_stage(160);
        uint32_t r1 = PSRAM_UNCACHED[0];
        mt_stage(170);
        /* Un seul printf groupé, APRÈS les accès : moins de pertes USB. */
        printf("[PSRAM] accès 32 bits : lu 0x%08lX, écrit 0x12345678, relu 0x%08lX (%s)\n",
               (unsigned long)r0, (unsigned long)r1, r1 == 0x12345678u ? "ok" : "ERREUR");
        sleep_ms(50);
    }

    /* 3) Taille */
    printf("[PSRAM] détection de taille...\n"); sleep_ms(50);
    mt_feed();
    int alias_bit = 0;
    uint32_t size = psram_detect_size(&alias_bit);
    printf("[PSRAM] détection terminée\n"); sleep_ms(50);
    if (alias_bit)
        printf("[PSRAM] repliement d'adresse au bit %d (offset +%lu Ko) → taille %lu Ko%s\n",
               alias_bit, (unsigned long)((1u << alias_bit) / 1024u), (unsigned long)(size / 1024u),
               size == MEMTEST_PSRAM_EXPECTED ? " (normal : fenêtre XIP 16 Mo > puce)"
                                              : " (≠ attendu : ligne d'adresse coupée/court-circuitée ?)");
    else
        printf("[PSRAM] aucun repliement jusqu'à +8 Mo → taille ≥ 16 Mo (fenêtre XIP = 16 Mo)\n");
    rep->psram_bytes = size;
    if (size != MEMTEST_PSRAM_EXPECTED)
        printf("[PSRAM] ⚠ taille détectée %lu Mo ≠ attendue %lu Mo\n",
               (unsigned long)(size / MB), (unsigned long)(MEMTEST_PSRAM_EXPECTED / MB));

    errlog_t e = {0};
    volatile uint32_t *m = PSRAM_UNCACHED;

    /* 4) Bus de données : un 1 / un 0 qui marche, mot de 32 bits. */
    {
        uint32_t base_i = 0x1000u / 4u;
        for (int b = 0; b < 32; b++) {
            uint32_t v = 1u << b;
            m[base_i] = v;
            uint32_t r = m[base_i];
            if (r != v) err_record(&e, "bus", 0x1000u, v, r);
            m[base_i] = ~v;
            r = m[base_i];
            if (r != ~v) err_record(&e, "bus", 0x1000u, ~v, r);
        }
        printf("[PSRAM] bus de données (walking 1/0, 32 bits) : %s\n",
               e.count ? "ERREUR" : "ok");
    }

    /* 5) Accès octet / demi-mot (écritures partielles du QMI). */
    {
        volatile uint8_t  *b8  = (volatile uint8_t  *)PSRAM_UNCACHED + 0x2000u;
        volatile uint16_t *b16 = (volatile uint16_t *)PSRAM_UNCACHED + 0x2000u / 2u + 8u;
        uint32_t before = e.count;
        volatile uint32_t *w = m + 0x2000u / 4u;
        w[0] = 0; w[1] = 0;
        b8[0] = 0x11; b8[1] = 0x22; b8[2] = 0x33; b8[3] = 0x44;
        if (w[0] != 0x44332211u) err_record(&e, "oct", 0x2000u, 0x44332211u, w[0]);
        b16[0] = 0xBEEF; b16[1] = 0xCAFE;
        volatile uint32_t *w2 = (volatile uint32_t *)(void *)b16;
        if (w2[0] != 0xCAFEBEEFu) err_record(&e, "dmot", 0x2010u, 0xCAFEBEEFu, w2[0]);
        printf("[PSRAM] accès octet / demi-mot : %s\n", e.count > before ? "ERREUR" : "ok");
    }

    /* 6) Passes plein-volume sur toute la taille détectée. */
    uint32_t words = size / 4u;
    for (int pass = 0; pass < MEMTEST_PSRAM_PASSES; pass++) {
        static const char *names[] = {"adresse", "inverse", "aléatoire"};
        uint32_t before = e.count;
        uint32_t rng = 0xA5A5F00Du;

        const uint32_t chunk = MB / 4u;   /* 1 Mo : un point de progression */

        printf("[PSRAM] passe %d/%d « %s » : écriture ", pass + 1, MEMTEST_PSRAM_PASSES, names[pass]);
        stdio_flush();
        uint64_t t0 = time_us_64();
        for (uint32_t i = 0; i < words; i++) {
            m[i] = psram_pattern(pass, i, &rng);
            if (((i + 1) % chunk) == 0) {
                mt_stage(500u + (uint32_t)pass * 100u + (i / chunk));   /* 5xx : écriture, Mo n */
                mt_feed(); putchar('.'); stdio_flush();
            }
        }
        uint64_t t1 = time_us_64();

        printf(" lecture ");
        stdio_flush();
        rng = 0xA5A5F00Du;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t exp = psram_pattern(pass, i, &rng);
            uint32_t got = m[i];
            if (got != exp) err_record(&e, "ram", i * 4u, exp, got);
            if (((i + 1) % chunk) == 0) {
                mt_stage(550u + (uint32_t)pass * 100u + (i / chunk));   /* 5xx : lecture, Mo n */
                mt_feed(); putchar('.'); stdio_flush();
            }
        }
        uint64_t t2 = time_us_64();
        printf("\n");

        printf("[PSRAM] passe %d/%d « %s » %lu Mo : %s — ",
               pass + 1, MEMTEST_PSRAM_PASSES, names[pass], (unsigned long)(size / MB),
               e.count > before ? "ERREURS" : "ok");
        psram_report_mbps("écriture", size, t1 - t0);
        psram_report_mbps(", lecture", size, t2 - t1);
        printf(" (sans cache)\n");
    }

    /* 7) Débit avec cache (zone 256 Ko, lecture séquentielle). */
    {
        const volatile uint32_t *c = PSRAM_CACHED;
        uint32_t n = (256u * 1024u) / 4u, acc = 0;
        uint64_t t0 = time_us_64();
        for (uint32_t i = 0; i < n; i++) acc += c[i];
        uint64_t t1 = time_us_64();
        printf("[PSRAM] lecture avec cache 256 Ko : ");
        psram_report_mbps("", 256u * 1024u, t1 - t0);
        printf(" (somme 0x%08lX)\n", (unsigned long)acc);
    }

    rep->psram_errors = e.count;
    rep->psram_ok = (e.count == 0) && (size == MEMTEST_PSRAM_EXPECTED);
    if (e.count)
        printf("[PSRAM] %lu erreur(s), bits de donnée touchés : masque 0x%08lX "
               "(bit n → ligne QSPI_SD(n mod 4) ; toujours le même bit = ligne fautive)\n",
               (unsigned long)e.count, (unsigned long)e.bitmask);
    printf("[PSRAM] RÉSULTAT : %s (%lu erreur(s), %lu Mo)\n",
           rep->psram_ok ? "OK" : "ÉCHEC", (unsigned long)e.count, (unsigned long)(size / MB));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Point d'entrée
 * ═══════════════════════════════════════════════════════════════════════ */

void memtest_run(memtest_report_t *rep) {
    memtest_report_t local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    rep->psram_cs_pin = -1;

    printf("\n========= MEMTEST (flash + PSRAM) =========\n");
    uint64_t t0 = time_us_64();
    /* Le boot précédent s'est-il figé pendant le test PSRAM ? */
    bool skip_psram = false;
    if (watchdog_hw->scratch[0] == MT_MAGIC) {
        uint32_t st = watchdog_hw->scratch[1];
        watchdog_hw->scratch[0] = 0;
        if (st >= 100) {
            printf("[MEMTEST] ⚠ le démarrage précédent s'est FIGÉ à l'étape %lu "
                   "(reset par watchdog : %s) — test PSRAM sauté cette fois.\n",
                   (unsigned long)st, watchdog_caused_reboot() ? "oui" : "non");
            printf("[MEMTEST]   100=avant lecture ID, 110=ID lu, 120=init QMI, 130=QMI ok,\n"
                   "[MEMTEST]   140=1re lecture, 150=écriture, 160=relecture, 170=accès ok,\n"
                   "[MEMTEST]   302..323=écriture marqueur bit k, 330=écriture offset 0,\n"
                   "[MEMTEST]   402..423=lecture marqueur bit k, 5xx=passes (500/550 = passe 1,\n"
                   "[MEMTEST]   600/650 = passe 2, 700/750 = passe 3 ; +n = Mo n)\n");
            skip_psram = true;
        }
    }

    flash_run(rep);
    if (skip_psram) {
        printf("\n[PSRAM] test sauté (figé au démarrage précédent). Recompiler/reflasher ou "
               "remettre la carte sous tension pour réessayer.\n");
    } else {
        /* Watchdog = filet de sécurité : s'il y a un blocage, la carte redémarre
         * et le boot suivant indique l'étape. */
        watchdog_enable(8000, true);
        psram_run(rep);
        hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
        watchdog_hw->scratch[0] = 0;   /* terminé normalement */
    }
    printf("\n[MEMTEST] durée totale : %lu ms\n", (unsigned long)((time_us_64() - t0) / 1000u));
    printf("===========================================\n\n");
}
