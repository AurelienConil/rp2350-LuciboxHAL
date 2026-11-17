/*
 * hal/memtest.h — Validation de la flash QSPI et de la PSRAM (bring-up)
 *
 * memtest_run() est appelée UNE fois au boot par main(), AVANT l'audio (les
 * opérations flash coupent les IRQ et le XIP : incompatible avec l'IRQ DMA
 * audio). Tout est loggé sur la console USB, préfixes [FLASH] / [PSRAM].
 *
 * FLASH (QMI CS0, XIP_BASE = 0x10000000)
 *   - ID JEDEC (fabricant / type / capacité), ID unique 64 bits, densité SFDP
 *   - sondage d'alias non destructif (la flash « boucle »-t-elle avant 16 Mo ?)
 *   - test écriture/effacement DESTRUCTIF sur le dernier secteur (4 Ko) de
 *     chaque tranche 1/2/4/8/16 Mo (détecte aussi une capacité plus petite que
 *     prévu, par repliement d'adresse), puis nettoyage. Ces secteurs sont en fin
 *     de flash, hors firmware (garde-fou : jamais sous __flash_binary_end).
 *   - temps d'effacement / programmation, débit de lecture XIP
 *   ⚠ Chaque démarrage use 2 cycles d'effacement par secteur testé (100 k cycles
 *     garantis) : mettre MEMTEST_ENABLE à 0 (ou MEMTEST_FLASH_WRITE à 0) une
 *     fois la validation faite.
 *
 * PSRAM APS6404L-SQN, 8 Mo (QMI CS1, 0x11000000 en cache, 0x15000000 sans cache)
 *   - broche CS1 : GPIO 0 ou 8 (19 = encodeur, 47 = pot A7 sur ce PCB) ;
 *     auto-détectée en lisant l'ID (cmd 0x9F) sur chaque candidate
 *   - init QMI (mode quad, 0xEB lecture / 0x38 écriture, timings APS6404-like)
 *   - détection de taille par repliement d'adresse, bus de données (1 qui
 *     marche), accès octet/demi-mot, puis 3 passes plein-volume (adresse,
 *     inverse, pseudo-aléatoire) avec masque des bits en erreur (→ ligne de
 *     donnée fautive) et débits.
 *
 * Rien n'est placé en PSRAM par l'éditeur de liens : c'est un test, pas un tas.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Réglages (surchargeables par -D... ou target_compile_definitions) ─────── */

/* 1 = lancer le test au boot. Mettre 0 une fois validé. */
#ifndef MEMTEST_ENABLE
#define MEMTEST_ENABLE            1
#endif

/* 1 = test flash destructif (erase/program des derniers secteurs). */
#ifndef MEMTEST_FLASH_WRITE
#define MEMTEST_FLASH_WRITE       1
#endif

/* Broche CS1 de la PSRAM : -1 = sonder GPIO 0 puis 8 ; sinon GPIO imposé
 * (CS1 n'existe que sur 0, 8, 19, 47). */
#ifndef MEMTEST_PSRAM_CS_PIN
#define MEMTEST_PSRAM_CS_PIN      (-1)
#endif

/* Fréquence max demandée à la PSRAM (Hz). Le diviseur QMI en découle
 * (150 MHz / 2 = 75 MHz avec 100 MHz). Baisser en cas d'erreurs de lecture. */
#ifndef MEMTEST_PSRAM_MAX_HZ
#define MEMTEST_PSRAM_MAX_HZ      100000000u
#endif

/* Capacité attendue de la PSRAM (octets) — APS6404L-SQN = 64 Mbit = 8 Mo
 * (datasheet Rev 4.0). Sert à signaler une différence. */
#ifndef MEMTEST_PSRAM_EXPECTED
#define MEMTEST_PSRAM_EXPECTED    (8u * 1024u * 1024u)
#endif

/* Nombre de passes plein-volume (1 = adresse, 2 = +inverse, 3 = +aléatoire). */
#ifndef MEMTEST_PSRAM_PASSES
#define MEMTEST_PSRAM_PASSES      3
#endif

typedef struct {
    /* Flash */
    bool     flash_ok;          /* tous les tests exécutés ont réussi */
    uint8_t  flash_jedec[3];    /* fabricant, type, capacité */
    uint32_t flash_bytes;       /* capacité retenue (JEDEC/SFDP), 0 si inconnue */
    uint32_t flash_errors;
    /* PSRAM */
    bool     psram_present;     /* ID lu sur une broche CS1 */
    bool     psram_ok;          /* tous les tests ont réussi */
    int      psram_cs_pin;      /* -1 si absente */
    uint32_t psram_bytes;       /* taille détectée */
    uint32_t psram_errors;
} memtest_report_t;

/* Lance l'ensemble (flash puis PSRAM), logue tout, remplit le rapport. */
void memtest_run(memtest_report_t *rep);

#ifdef __cplusplus
}
#endif
