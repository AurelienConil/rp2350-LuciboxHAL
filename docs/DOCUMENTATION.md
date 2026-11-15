# rp2350-LuciboxHAL — Documentation d'architecture

> Document de référence interne. Public visé : développeur confirmé (C/C++,
> embarqué, registres, DMA/IRQ). Ce n'est **pas** une introduction à l'audio
> numérique — I2S, PCM, dB, oversampling etc. sont supposés connus. L'objectif
> ici est de documenter les choix d'architecture de **ce dépôt précis**, le
> contrat entre ses couches, et le "pourquoi" derrière chaque décision non
> évidente.

---

## Table des matières

1. [Vue d'ensemble](#1-vue-densemble)
2. [Le HAL audio](#2-le-hal-audio)
3. [Le driver ES8311](#3-le-driver-es8311)
4. [Les contrôles (controls.h/.c)](#4-les-contrôles-controlshc)
5. [Le contexte audio — format, buffers, contraintes temps réel](#5-le-contexte-audio--format-buffers-contraintes-temps-réel)
6. [Le système d'instruments](#6-le-système-dinstruments)
7. [Modèle template — un instrument, un projet](#7-modèle-template--un-instrument-un-projet)
8. [Build & toolchain](#8-build--toolchain)
9. [Pièges connus — référence condensée](#9-pièges-connus--référence-condensée)
10. [Roadmap / points d'extension](#10-roadmap--points-dextension)

---

## 1. Vue d'ensemble

### 1.1 Objectif

`rp2350-LuciboxHAL` est un **template GitHub** pour firmware audio RP2350
(Cortex-M33, Pico 2) piloté par un codec **ES8311** (DAC + ADC mono, I2C +
I2S). Le projet a deux couches strictement séparées :

- une **HAL** (`hal/`) qui gère tout ce qui touche au silicium : I2C, I2S
  (PIO), DMA, PWM (MCLK), ADC interne pour les potentiomètres ;
- un **instrument** (`instruments/`) qui consomme cette HAL via une
  interface abstraite unique et implémente la logique audio.

Le template contient un seul instrument, `SelfTest` : sweep sinusoïdal sur
la sortie DAC + passthrough du signal ADC, mixés — pas de logique musicale,
juste une preuve physique que la chaîne complète (I2C, MCLK, PIO I2S
duplex, DMA) fonctionne dans les deux sens. Chaque projet dérivé du
template (via "Use this template" sur GitHub) remplace cet instrument par
le sien, sans toucher à la HAL. Il n'y a pas de sélection multi-instrument
au runtime ni à la compilation — un seul `.cpp` d'instrument est listé
dans `CMakeLists.txt` (§7). Il n'y a pas de séquenceur, pas d'OS, pas de
scheduler — juste une IRQ DMA qui bat la mesure et un `main()` qui fait
l'init puis dort dans une boucle de polling basse fréquence.

### 1.2 Stack matérielle

| Composant | Rôle | Interface |
|---|---|---|
| RP2350 (Pico 2) | MCU, génère MCLK, PIO I2S, DMA, ADC pots | — |
| ES8311 | Codec audio mono : 1 DAC (sortie), 1 ADC (entrée ligne) | I2C1 (config) + I2S (data) |
| 8× potentiomètres, 10 boutons (4 lumineux), 4 encodeurs | Contrôles temps réel | ADC interne (GPIO 40-47), GPIO 18-39 |
| MPR121 (prévu, stub) | Pavé tactile 12 touches | I2C1 (partagé avec ES8311) |

Le brochage complet est dans `CLAUDE.md` — pas dupliqué ici, il ne change
quasiment jamais alors que ce document, lui, doit rester lisible sans être
un miroir exact des fichiers `.h`.

### 1.3 Flux de données — vue d'ensemble

```
                    ┌──────────────────────────────────────────────┐
                    │                   RP2350                      │
                    │                                                │
                    │                 AudioInstrument::process()     │
                    │                    ▲              │            │
   ES8311 ADC ──────┼── PIO0 SM1(RX) ────┘              └──► PIO0 SM0(TX) ──── ES8311 DAC
   (GPIO13 ADCDAT)  │        ▲                                    │            (GPIO12 DACDAT)
                    │        │  DMA RX (ch1)      DMA TX (ch0)    │
                    │        └──────── rx_buf[] ──────► tx_buf[] ─┘
                    │                       (IRQ DMA_IRQ_0)
                    │                                                │
   pots/boutons ────┼──► controls_update() (cache) ──────────────────┤
   (main(), ~50 Hz) │        ▲ lu en O(1) par process() via           │
                    │        controls_pot_get()/controls_button_get() │
                    └──────────────────────────────────────────────┘
```

Le pipeline audio est **entièrement matériel entre les buffers RAM et les
broches** (PIO + DMA), le CPU n'intervenant que dans la callback `process()`
appelée toutes les ~5,3 ms. Voir §2 pour le détail. Les capteurs (pots,
boutons, tactile I2C) sont scannés **indépendamment**, depuis la boucle
`main()` à ~50 Hz, jamais depuis cette IRQ — voir §4.

### 1.4 Organisation du code

```
rp2350-LuciboxHAL/
├── main.cpp                    orchestration : init séquencée + boucle main basse fréquence
├── i2s_duplex.pio              2 programmes PIO : i2s_tx (master) + i2s_rx (slave synchronisé)
├── CMakeLists.txt              sources, libs SDK
├── hal/
│   ├── audio.h/.c              MCLK PWM, PIO I2S, DMA double-buffer, dispatch callback
│   ├── es8311.h/.c             driver registres ES8311 (I2C)
│   └── controls.h/.c           pots, boutons, LEDs, encodeurs (timer 1 kHz) + stub MPR121
└── instruments/
    ├── audio_instrument.hpp    interface abstraite (contrat commun)
    └── selftest.hpp/.cpp       instrument fake : sweep DAC + passthrough ADC
```

Aucune couche n'a de dépendance circulaire : `instruments/` dépend de
`hal/` (constantes, prototypes), jamais l'inverse. `hal/audio.c` ne connaît
même pas l'existence de la classe `AudioInstrument` — il ne voit qu'un
pointeur de fonction C et un `void *ctx` opaques (voir §7.2).

---

## 2. Le HAL audio

### 2.1 Rôle et contrat

`hal/audio.h` expose une API volontairement étroite :

```c
void audio_hal_init(audio_process_fn fn, void *ctx);
void audio_hal_start(void);
void mclk_init(void);
```

Le contrat côté appelant (`main.cpp`) est une séquence stricte (détaillée en
§2.6). Le contrat côté callback (`audio_process_fn`) est celui d'un
handler d'IRQ (détaillé en §5.3). En dehors de ces deux contrats, le HAL ne
fait aucune hypothèse sur ce qui tourne dessus — c'est ce qui permet à
`instruments/` d'être totalement découplé du HAL bas niveau.

Le HAL ne gère **pas** :
- le mixage ou le traitement du signal (zéro DSP dans `hal/`) ;
- l'état applicatif (volume, fréquence...) — c'est la responsabilité de
  l'instrument ;
- le multi-instrument runtime — un seul `audio_process_fn`/`ctx` enregistré,
  fixé une fois pour toutes à `audio_hal_init()`.

### 2.2 Pipeline matériel

```
GPIO14 (MCLK, PWM 12.5 MHz)
         │
         ▼
      ES8311 ── I2C1 (config registres) ──── RP2350
         │
         │  I2S : BCLK(10) / LRCK(11) / DACDAT(12) / ADCDAT(13)
         ▼
      PIO0 SM0 (i2s_tx, master : génère BCLK+LRCK, pousse DACDAT)
      PIO0 SM1 (i2s_rx, esclave synchronisé : échantillonne ADCDAT)
         │                                    ▲
         ▼ TX FIFO                            │ RX FIFO
      DMA ch0 (RAM→FIFO)                DMA ch1 (FIFO→RAM)
         │                                    │
         ▼                                    ▼
      tx_buf[active][256]              rx_buf[active][256]
```

Deux state machines PIO partagent le même diviseur d'horloge et sont
démarrées **simultanément** (`pio_enable_sm_mask_in_sync`), sans quoi RX et
TX dérivent l'un par rapport à l'autre de quelques cycles PIO — assez pour
casser le sampling du signal ADC (voir §2.3).

### 2.3 PIO I2S duplex (`i2s_duplex.pio`)

**Programme TX (`i2s_tx`, SM0)** — master I2S. `.side_set 2` pilote BCLK
(bit 0) et LRCK (bit 1) simultanément à chaque instruction. Boucle par canal
(gauche puis droit), 32 cycles SM par canal = 64 cycles par trame stéréo.
Le point non trivial : **LRCK doit basculer sur le front descendant du
dernier bit de BCLK**, pas sur le front montant — c'est une exigence I2S
Philips que l'ES8311 applique strictement (violation setup → silence total,
documenté en détail dans `CLAUDE.md` et dans les commentaires du `.pio`
lui-même). Ne pas retoucher ce timing sans relire ces deux sources.

Autopull 32 bits, shift gauche (MSB first) : un mot DMA = une trame stéréo
`[L:16][R:16]`, ce qui simplifie énormément le contrat de buffer côté C
(§5.1).

**Programme RX (`i2s_rx`, SM1)** — esclave *logique* mais généré et cadencé
en interne comme TX : il ne lit pas BCLK/LRCK sur des pins, il rejoue le
même minutage via son propre `side_set 1` non connecté à un GPIO (`side-set`
utilisé uniquement pour le timing interne du programme, pas pour driver une
broche — cf. commentaire dans le `.pio`). Il échantillonne `ADCDAT` sur le
front **montant** de BCLK, soit un demi-cycle après que TX ait mis à jour
DACDAT sur le front descendant — cohérent avec le fait que l'ES8311 présente
sa sortie DO sur le même flanc descendant que celui utilisé par TX pour ses
propres bits.

Les deux programmes utilisent le **même `clkdiv`**, calculé à partir de
`sample_rate` et `bits` (`sys_clk / (2 × Fs × bits × 2 canaux)`). Le
`i2s_rx_program_init()` doit être appelé après `i2s_tx_program_init()` et
les deux SM démarrés en synchronisation — c'est un invariant du fichier PIO
que le HAL respecte à la lettre dans `audio_hal_start()`.

### 2.4 DMA double-buffer ping-pong

Deux tableaux `int32_t[2][256]` pour TX et RX (`tx_buf`, `rx_buf`,
`hal/audio.c:33-34`). Un seul canal DMA par direction, reconfiguré à chaque
IRQ plutôt qu'un DMA chaîné — plus simple à raisonner, coût négligeable vu
que la reconfiguration prend une poignée de cycles face aux ~5,3 ms
disponibles.

Le canal **TX** est celui qui porte l'IRQ (`dma_channel_set_irq0_enabled`
sur `dma_ch_tx` uniquement). Le canal RX n'a pas d'IRQ propre : il est
relancé *depuis* le handler de l'IRQ TX. C'est un choix délibéré — un seul
point de synchronisation, pas de risque de race entre deux handlers qui
se chevauchent.

Séquence dans `dma_irq_handler()` (`hal/audio.c`) :

1. acquitte l'IRQ TX ;
2. bascule `active_buf` (0↔1) ;
3. relance TX sur `tx_buf[active_buf]` — **ce buffer a été rempli par
   l'appel de callback précédent**, pas par celui qu'on va faire maintenant ;
4. relance RX sur `rx_buf[active_buf]` ;
5. convertit `rx_buf[just_done]` (trame stéréo I2S) en `mono_rx_buf` (§5.1) ;
6. appelle `g_process_fn(mono_rx_buf, mono_tx_buf, ...)` ;
7. convertit `mono_tx_buf` en `tx_buf[just_done]` (trame stéréo I2S).

Aucun capteur n'est lu ici. C'était le cas des pots dans une version
antérieure (`controls_update()` était appelée à chaque IRQ) — ça a été
délibérément retiré : la lecture de capteurs (ADC ou future I2C bloquante)
n'a rien à faire dans le chemin le plus critique du système, et son rythme
(50 Hz, cf. §4) n'a aucun rapport avec celui de l'audio (188 Hz). Voir §4
pour où ça vit maintenant.

`dma_irq_handler()` est marqué `__not_in_flash_func()` — il tourne en SRAM, pas
depuis le cache flash XIP. Objectif : éliminer toute latence variable due à un
cache miss flash dans le handler le plus critique du système. `controls_update()`
et les lectures ADC/GPIO qu'elle déclenche ne sont **pas** marquées ainsi : elles
ne sont plus sur ce chemin chaud (voir §4).

Le point clé à comprendre pour ne pas se tromper en modifiant ce code :
**au moment où la callback s'exécute, le hardware travaille déjà sur
l'autre moitié du double-buffer.** La callback a donc toute la durée d'un
buffer (~5,3 ms) pour remplir `tx_buf[just_done]`, qui ne sera consommé par
le DMA qu'au *prochain* cycle IRQ. C'est ce qui donne la marge temps réel —
sans ce décalage d'une génération, la callback devrait s'exécuter en un
temps quasi nul.

`audio_hal_start()` pré-remplit `tx_buf[1]` par un appel **synchrone** de la
callback avant même de démarrer les SM PIO (`hal/audio.c:181-182`), pour
que le tout premier cycle DMA ait déjà un buffer TX valide plutôt que le
silence du `memset` initial.

### 2.5 Contrat de la callback

Voir §5.3 pour le détail des contraintes temps réel (identiques à celles
d'un handler d'IRQ classique RP2350 : pas d'allocation, pas de blocking
call, budget < 5,3 ms).

### 2.6 Séquence d'initialisation — pourquoi cet ordre est non négociable

`main.cpp` documente huit étapes strictement ordonnées. Les contraintes
réelles derrière chaque transition :

1. **I2C avant MCLK** — ⚠ **hypothèse non validée, pas un fait établi**
   (voir CLAUDE.md §"Pièges connus" pour le détail). L'explication reprise
   de `rp2350-BasicSine` est que le PWM MCLK (12,5 MHz sur GPIO14) génère
   des perturbations EMI qui corrompraient les transactions I2C si le bus
   tourne en même temps que l'établissement des registres ES8311. Aucun
   test EMI (oscillo, spectre, essai en ordre inversé) n'a jamais été fait
   pour le confirmer — la cause réelle d'un éventuel échec pourrait être
   tout autre chose (timing CSM, race condition logicielle...). L'ordre
   I2C avant MCLK est conservé par précaution : toute la configuration I2C
   (`es8311_init()`) se fait avant `mclk_init()`, sans que la raison
   invoquée soit vérifiée.
2. **ES8311 registres avant Controls/HAL** — pas de contrainte technique
   forte ici, mais `es8311_init()` doit renvoyer `true` (chip détecté) avant
   de continuer, sans quoi le firmware s'arrête (`while(true)
   tight_loop_contents()`), donc autant le faire tôt.
3. **Controls avant HAL audio** — indépendant en théorie, ordre choisi pour
   que tout ce qui est "lecture seule, pas de démarrage de flux" soit prêt
   avant que le flux audio ne démarre.
4. **HAL audio init (PIO+DMA) *sans démarrer*** — les SM PIO et les canaux
   DMA sont configurés mais `pio_sm_set_enabled`/`dma_channel_start` ne sont
   PAS appelés ici. Le HAL est "armé" mais silencieux.
5. **Instrument init()** — l'instrument peut faire des opérations lentes
   (calcul LUT, `printf`, allocation statique) car on n'est pas encore dans
   un contexte IRQ.
6. **MCLK** — activé seulement maintenant, après que I2C ait fini tout son
   travail de configuration.
7. **ES8311 CSM_ON** (`REG 0x00 = 0x80`) — la Chip State Machine de l'ES8311
   transite S0→S7 (fully powered) **seulement si MCLK est stable en
   entrée**. L'activer avant MCLK stable bloque la CSM à un état
   intermédiaire (silence, ou pire, comportement indéfini). `es8311_power_up()`
   attend 50 ms et relit le registre de statut CSM pour vérifier `S7`.
8. **`audio_hal_start()`** — démarre DMA + PIO simultanément. C'est
   seulement à cette étape que le flux audio commence réellement à
   transiter.

Cette séquence n'est pas un détail d'implémentation arbitraire : c'est la
résolution d'un graphe de dépendances hardware (EMI *supposée* — non
vérifiée, cf. point 1 — power sequencing CSM, synchronisation SM) qui a été
découvert empiriquement (cf. les `printf` de diagnostic à chaque étape et
la relecture du registre CSM comme vérification). Toute réorganisation
doit repasser par ces contraintes ; celle liée à l'EMI mériterait en plus
un vrai test de validation avant d'être considérée comme acquise.

### 2.7 MCLK — limite connue

`mclk_init()` génère 12,5 MHz via PWM (`sys_clk/12`, wrap=11, duty 50%). La
cible exacte pour un ratio MCLK/Fs=256 à 48 kHz serait 12 288 000 Hz — l'écart
(+1,7 %) se traduit directement par un décalage de pitch proportionnel
(440 Hz réel ≈ 447 Hz perçu). Acceptable pour un POC, documenté comme tel
dans le code. Pour une version production, il faudrait passer par un
`clk_gpout` avec diviseur fractionnaire pour tomber exactement sur
12 288 000 Hz (ou changer le ratio MCLK/Fs vers une valeur qui tombe pile
avec un diviseur PWM entier).

---

## 3. Le driver ES8311

### 3.1 Rôle

`hal/es8311.c` est un driver registres pur : pas d'état interne caché en
dehors des registres du chip lui-même, pas de FSM logicielle. Deux
fonctions structurantes :

- `es8311_init()` — écrit tous les registres DAC + ADC, **CSM_ON=0**
  (le chip reste éteint côté CSM). Vérifie le chip ID (`0x83 0x11`) avant
  toute écriture ; retourne `false` sinon (ce qui déclenche l'arrêt fatal
  dans `main.cpp`).
- `es8311_power_up()` — bascule CSM_ON=1 et attend la transition S0→S7.

Le détail registre par registre (OSR, formats I2S, alimentation analogique,
gain PGA...) est commenté exhaustivement dans `hal/es8311.c` lui-même et
dans `CLAUDE.md` §"Registres ES8311 clés" / §"Pièges connus" — ce document
ne le duplique pas, il documente uniquement la structure et le pourquoi de
la séparation `init()`/`power_up()`.

### 3.2 Pourquoi séparer `init()` et `power_up()`

C'est directement lié à la contrainte MCLK du §2.6 : la CSM ne peut
atteindre son état stable qu'avec une horloge MCLK présente. Si le driver
faisait tout en un seul appel, il n'y aurait aucun moyen d'insérer
`mclk_init()` au bon endroit dans la séquence. Cette séparation en deux
fonctions publiques est donc le point d'ancrage qui permet à `main.cpp`
d'orchestrer l'ordre I2C→registres→MCLK→CSM.

### 3.3 API runtime

`es8311_set_dac_volume(float vol_db)` et `es8311_set_mic_gain(uint8_t
gain)` sont des setters directs de registre (pas de rampe logicielle, pas
de lissage). Elles sont **bloquantes** (I2C synchrone) — donc à n'appeler
que depuis le main loop ou l'init, jamais depuis la callback audio IRQ
(cohérent avec la contrainte générale du §5.3).

### 3.4 Pièges — renvoi

Les pièges spécifiques ES8311 (OSR minimum 0x10, logique inversée de
PDN_VREF, bit ANACLKDAC_ON obligatoire...) sont propres au silicium et déjà
documentés de façon exhaustive dans `CLAUDE.md`. Ce document n'en fait
qu'un résumé en §9 — la source de vérité reste `CLAUDE.md` et les
commentaires inline de `es8311.c`.

---

## 4. Les contrôles (`controls.h/.c`)

### 4.1 Architecture générale — un seul scan, hors IRQ audio, ~50 Hz

Pots, boutons et (à terme) tactile capacitif I2C sont tous scannés par une
**unique fonction**, `controls_update()`, appelée depuis la boucle `main()`
toutes les `CONTROLS_UPDATE_PERIOD_MS` (20 ms, soit 50 Hz — `controls.h`,
constante partagée entre `main.cpp` et `controls.c` pour ne pas dupliquer le
rythme). Ce n'était pas le cas à l'origine : les pots étaient lus **depuis
l'IRQ DMA**, à chaque bloc (~188 Hz) — bien plus vite que nécessaire pour un
geste humain, et ça mettait des conversions ADC dans le chemin le plus
critique du système. C'est ce qui a été corrigé : l'IRQ audio ne touche
**plus jamais** un capteur, quel qu'il soit — voir §2.4.

Ce découplage est ce qui permettra d'ajouter l'I2C bloquant (MPR121, §4.4)
au même scan que les pots sans risque : une transaction I2C dans l'IRQ ferait
exploser le budget de 5,3 ms, mais dans la boucle `main()` à 50 Hz elle a 20 ms
de marge.

### 4.2 Contrôles du PCB — pots, boutons, LEDs, encodeurs

Brochage (tables en tête de `hal/controls.c`, détail dans `hal/controls.h`) :

| Contrôle | Nb | GPIO |
|----------|----|------|
| Pots A0-A7 | 8 | 40-47 (ADC ch0-7, RP2350B) |
| Boutons simples | 6 | 26-31 (hypothèse : contigus) |
| Boutons lumineux (switch / LED) | 4 | 32/33, 34/35, 36/37, 38/39 |
| Encodeurs (A,B, sans push) | 4 | 18/19, 20/21, 22/23, 24/25 |

Les boutons partagent un seul espace d'indices 0-9 (0-5 simples,
`CONTROLS_BTN_LIT(n)` = 6+n pour les lumineux).

**Scan.** Pots + boutons + MPR121 : `controls_update()` depuis la boucle
`main()` à 50 Hz. Encodeurs : timer répétitif 1 kHz (un scan à 50 Hz perdrait
des pas en rotation rapide), table de transitions quadrature,
`CONTROLS_ENCODER_STEPS_PER_DETENT` (4 par défaut) transitions par cran.

**API côté audio** (tout est O(1), sans accès matériel, utilisable dans `process()`) :

```c
float   controls_pot_get(uint8_t ch);            // 0..1, lissé (~15 ms)
bool    controls_button_get(uint8_t i);          // niveau débounced, true = appuyé
bool    controls_button_pressed(uint8_t i);      // front montant, une fois par appui
bool    controls_button_released(uint8_t i);     // front descendant
int32_t controls_encoder_delta(uint8_t i);       // crans depuis le dernier appel, signé
void    controls_led_set(uint8_t n, bool on);    // LED du bouton lumineux n
```

Exemple dans un instrument :
```cpp
void process(const int16_t *rx, int16_t *tx, uint32_t n) {
    float vol = controls_pot_get(0);
    cutoff_ += controls_encoder_delta(0) * 50.0f;
    if (controls_button_pressed(CONTROLS_BTN_LIT(0))) {
        rec_ = !rec_;  controls_led_set(0, rec_);
    }
    ...
}
```

**Concurrence.** Chaque variable partagée n'a qu'un écrivain : le scan écrit
niveaux et compteurs d'événements (`press_count`...), le lecteur écrit ses
compteurs "déjà vus" — donc pas de lecture-modification-écriture partagée
entre main et IRQ audio. Conséquence : fronts et deltas sont
**consommés à la lecture, par un seul lecteur** (le `process()`).

Lissage pots : filtre un pôle, coefficient calculé à l'init depuis
`CONTROLS_POT_SMOOTH_TIME_MS`. Boutons : anti-rebond sur
`CONTROLS_BUTTON_DEBOUNCE_SAMPLES` lectures identiques (~40 ms). Polarité
(pull-up, actif bas) masquée par l'API. Ne jamais appeler
`controls_pot_read_raw()`/`_norm()` depuis `process()`.

### 4.4 MPR121 — stub

Interface définie mais non implémentée (`controls_mpr121_init/update/
touched` retournent des valeurs par défaut). Le point de conception
important, déjà posé même si le hardware n'est pas branché : **le polling
MPR121 doit se faire depuis le main loop, jamais depuis l'IRQ DMA**, parce
que les transactions I2C sont bloquantes et dureraient bien plus que le
budget de 5,3 ms. `controls_mpr121_update()` est appelée depuis
`controls_update()` (donc déjà au bon endroit, à 50 Hz) en attente de
l'implémentation réelle — il n'y a plus de boucle 100 Hz séparée comme
avant, tout est unifié dans le même scan (§4.1). `controls_mpr121_touched()`
renvoie un `uint16_t` mis à jour de façon atomique (lecture safe même si
appelée depuis l'IRQ une fois l'implémentation faite, car un accès aligné
16 bits sur Cortex-M est atomique sans lock).

---

## 5. Le contexte audio — format, buffers, contraintes temps réel

### 5.1 Format des échantillons — mono côté instrument, stéréo sur le fil

Le codec ES8311 est **mono** (un seul DAC, un seul ADC), mais le protocole I2S
Philips transporte toujours une trame stéréo de 2×16 bits (`i2s_duplex.pio`,
timing figé — §2.3). Cette dualité est résolue une fois pour toutes dans
`hal/audio.c` : `audio_process_fn`/`AudioInstrument::process()` ne voient
jamais qu'un flux **mono `int16_t`**, jamais de trame `[L:16][R:16]`.

La conversion vit dans deux fonctions `static inline` d'`hal/audio.c`,
résolues par `#if` à la compilation (coût runtime nul) :

```c
// stéréo (fil I2S) -> mono (vu par l'instrument)
static inline int16_t unpack_mono_rx(int32_t frame) {
#if AUDIO_RX_SLOT == AUDIO_SLOT_RIGHT
    return (int16_t)(frame & 0xFFFF);
#else
    return (int16_t)(frame >> 16);
#endif
}

// mono (vu par l'instrument) -> stéréo (fil I2S)
static inline int32_t pack_mono_tx(int16_t s) {
    uint16_t u = (uint16_t)s;
#if AUDIO_TX_SLOT == AUDIO_SLOT_LEFT
    return ((int32_t)u << 16);
#elif AUDIO_TX_SLOT == AUDIO_SLOT_RIGHT
    return (int32_t)u;
#else /* AUDIO_SLOT_BOTH */
    return ((int32_t)u << 16) | u;
#endif
}
```

`AUDIO_RX_SLOT`/`AUDIO_TX_SLOT` (`hal/audio.h`) choisissent quel(s) slot(s) I2S
portent le signal mono — par défaut lecture sur L, écriture dupliquée sur L+R.
Redéfinissables via `target_compile_definitions` si un futur câblage diffère.
Deux buffers `int16_t[AUDIO_BUFFER_FRAMES]` (`mono_rx_buf`/`mono_tx_buf`,
`hal/audio.c`) servent de zone d'échange à cette frontière, réutilisés de façon
synchrone à chaque IRQ (pas besoin de double-buffer ici : la conversion est
séquentielle, dans le même appel).

Un instrument n'a donc plus jamais à écrire de `>> 16` ni de `<< 16 | s` — ce
détail est entièrement absorbé par la HAL, une fois pour tous les projets
dérivés.

### 5.2 Taille de buffer et latence

`AUDIO_BUFFER_FRAMES = 256` @ 48 kHz → 5,33 ms par demi-buffer. Avec le
double-buffering (§2.4), la latence bout-en-bout entrée→sortie est de
l'ordre de 2 demi-buffers, soit ~10,6 ms — non configurable dynamiquement,
seulement à la compilation via `AUDIO_BUFFER_FRAMES`.

### 5.3 Contraintes strictes dans `process()`

Reprises du contrat IRQ documenté dans `audio_instrument.hpp` et
`audio.h` :

- budget < 5,3 ms (sinon le buffer suivant n'est pas prêt à temps → glitch
  audio, pas de mécanisme de récupération) ;
- pas d'allocation dynamique (`new`, `malloc`) — tous les buffers
  d'instrument sont des membres statiques dimensionnés à la compilation
  (voir `SelfTest::lut_`, taille fixe `LUT_SIZE`) ;
- pas de `printf`, pas de `sleep_ms`, pas de mutex bloquant ;
- lecture des pots/boutons : uniquement via `controls_pot_get()`/
  `controls_button_get()` (cache, O(1), pas d'accès matériel — voir §4) ;
  jamais `controls_pot_read_raw()`/`_norm()` ;
- lecture MPR121 : accès atomique du bitmask, OK en lecture seule.

Rien dans le code n'impose ces contraintes par construction (pas de
vérification statique, pas de watchdog sur le temps d'exécution de la
callback) — elles sont uniquement documentées et respectées par discipline
dans `SelfTest`. Un futur instrument qui les violerait ne serait pas détecté à
la compilation, seulement à l'écoute (glitches) — à l'exception de la lecture
des pots, où la discipline "une fois par bloc" est désormais mécanique
(§4.2) : ce n'est plus l'instrument qui décide quand l'ADC est lu.

### 5.4 Entrée ADC optionnelle

Le pointeur `rx` peut être `NULL` "si l'ADC n'est pas activé" d'après le
commentaire d'en-tête (`audio.h:59`) — mais dans l'implémentation actuelle,
RX est toujours initialisé et démarré en même temps que TX
(`audio_hal_init`/`audio_hal_start` n'ont pas de mode "TX only"). Ce `NULL`
documenté est donc un contrat d'API pour une extension future, pas un cas
qui se produit actuellement. `SelfTest::process` lit activement `rx` (c'est
tout l'intérêt de l'instrument : prouver le chemin ADC) — un instrument
dérivé purement génératif peut l'ignorer avec `(void)rx;`, comme le faisait
l'ancien `Synth`.

---

## 6. Le système d'instruments

### 6.1 L'interface `AudioInstrument`

```cpp
class AudioInstrument {
public:
    virtual ~AudioInstrument() = default;
    virtual void init() = 0;
    virtual void process(const int16_t *rx, int16_t *tx, uint32_t n_frames) = 0;
    virtual const char *name() const = 0;
};
```

Trois méthodes, contrat asymétrique :

- `init()` s'exécute **hors IRQ**, dans `main()`, avant `audio_hal_start()`.
  Toute opération lente ou non temps-réel (calcul de LUT, `printf`,
  `memset` de gros buffers) va ici.
- `process()` s'exécute **dans l'IRQ DMA**, avec les contraintes du §5.3.
- `name()` est purement cosmétique (log de démarrage).

Pas de méthode `deinit()`/`stop()` — cohérent avec le fait qu'un seul
instrument est actif pour toute la durée de vie du firmware (§7). Pas de
méthode de configuration/paramétrage générique non plus : l'instrument lit
ses propres pots directement dans `process()`, il n'y a pas de couche
d'abstraction "paramètres" partagée.

### 6.2 Cycle de vie

```
main() :
  instancié en static (avant main, durée de vie = programme entier)
      │
      ▼
  g_instrument.init()          ── une fois, hors IRQ
      │
      ▼
  audio_hal_start()
      │
      ▼
  [IRQ DMA toutes les 5,3ms] → g_instrument.process(rx, tx, 256)   ── boucle infinie
```

L'instrument ne se détruit jamais (pas de `delete`, objet `static` à durée
de vie programme). Il n'y a pas de mécanisme de reset ou de
réinitialisation à chaud d'un instrument déjà actif.

### 6.3 Remplacer l'instrument du template — procédure concrète

`SelfTest` n'est pas destiné à rester : c'est le point de départ que
chaque projet dérivé remplace par son propre instrument (synth, sampler,
looper...). Étapes pour ajouter, par exemple, un instrument `Arpeggiator` :

1. **Fichiers** — créer `instruments/arpeggiator.hpp` et
   `instruments/arpeggiator.cpp`, en suivant le squelette de
   `selftest.hpp/cpp`.
2. **Héritage** — `class Arpeggiator : public AudioInstrument`, implémenter
   les trois méthodes virtuelles pures. Tout état (LUT, buffers, positions)
   est membre privé de la classe, dimensionné statiquement (pas de
   `std::vector`, pas d'allocation dynamique — cf. §5.3).
3. **CMakeLists.txt** — remplacer `instruments/selftest.cpp` par
   `instruments/arpeggiator.cpp` dans la liste
   `add_executable(rp2350-LuciboxHAL ...)` (`CMakeLists.txt:33-42`).
4. **main.cpp** — remplacer l'`#include "instruments/selftest.hpp"` et la
   déclaration `static SelfTest g_instrument;` par l'équivalent pour
   `Arpeggiator`.
5. **Compiler** — recompilation via l'extension VS Code uniquement (règle
   projet, voir `CLAUDE.md`).

Aucune autre partie du code n'a besoin d'être touchée : la HAL, ES8311 et
`controls` sont agnostiques à l'instrument actif — c'est précisément le
but de l'interface abstraite (§7.1). C'est aussi ce qui rend le modèle
template viable : un seul instrument par projet dérivé, zéro dépendance
entre son code et celui de la HAL.

### 6.4 Étude de cas — `SelfTest`

Deux briques combinées, chacune prouvant un chemin matériel distinct :

- **Sweep DAC** — oscillateur à table de correspondance (LUT) 256 entrées +
  accumulateur de phase 32 bits (pattern DDS classique), balayé entre
  440 et 470 Hz par un LFO à 0,1 Hz (`update_sweep_()`, appelé une fois par
  buffer). L'indexation LUT utilise les 8 bits de poids fort de
  l'accumulateur (`phase_acc_ >> 24`) — le débordement naturel de
  l'`uint32_t` gère le wrap de phase sans `if`/`%`. Audible en continu, même
  en silence total côté entrée : c'est la preuve que le chemin DAC (I2S TX,
  PIO, ES8311 DAC) fonctionne.
- **Passthrough ADC** — échantillon mono `rx[i]` (déjà extrait du slot I2S par
  la HAL, §5.1 — plus de `>> 16` côté instrument), multiplié par un gain fixe
  et ajouté au sweep. Parler dans le micro ou injecter un signal ligne doit
  s'entendre en plus du sweep : c'est la preuve que le chemin ADC (ES8311 ADC,
  PIO RX, DMA) fonctionne.

Les deux contributions sont sommées puis clampées à la plage `int16_t`
(`SWEEP_GAIN = PASSTHROUGH_GAIN = 0.5`, marge volontaire avant clamp pour
éviter la saturation quand les deux signaux sont forts simultanément). Pas
de dépendance aux pots — un instrument de preuve n'a rien à paramétrer.

---

## 7. Modèle template — un instrument, un projet

### 7.1 Un seul instrument, jamais de sélection runtime ni compile-time

`main.cpp` déclare **un seul objet global statique** — `g_instrument`, de
type `SelfTest` (`main.cpp:41`) — sans macro de sélection ni `#if/#elif`.
Il n'existe **qu'un seul instrument dans le binaire**, et un seul fichier
`.cpp` d'instrument dans les sources CMake (`CMakeLists.txt:36-42`).

Pourquoi pas un registre runtime d'instruments (tableau de pointeurs,
sélection par bouton) ou un flag de compilation multi-mode (l'ancien
`APP_MODE`, abandonné avec le passage en template) :

- **Empreinte mémoire minimale** — un seul `AudioInstrument` alloué
  statiquement, pas de coût pour du code non utilisé (pertinent dès qu'un
  instrument dérivé bufferise de l'audio en SRAM, cf. `CLAUDE.md`
  §"Pièges connus").
- **Simplicité de l'init séquencée** — la séquence à 8 étapes du §2.6
  initialise et démarre un seul flux ; un switch runtime demanderait de
  définir un protocole d'arrêt propre (drainer le DMA, réinitialiser
  l'état interne de l'instrument précédent, etc.) qui n'existe pas.
- **Le modèle template pousse la variabilité au niveau du dépôt, pas du
  binaire** — au lieu d'un firmware multi-instrument sélectionnable, chaque
  instrument vit dans son propre projet dérivé (`instruments/selftest.cpp`
  remplacé, §6.3), avec la HAL commune héritée du template et non dupliquée
  ni ré-écrite à chaque fois.

### 7.2 Le mécanisme de dispatch

Le pont entre C (le HAL, `hal/audio.c`) et C++ (les instruments,
polymorphisme) se fait via une callback C **libre**, définie dans
`main.cpp` :

```cpp
extern "C" void audio_cb(const int16_t *rx, int16_t *tx, uint32_t n, void *ctx) {
    static_cast<AudioInstrument *>(ctx)->process(rx, tx, n);
}
```

`audio_hal_init(audio_cb, &g_instrument)` enregistre cette fonction et le
pointeur `&g_instrument` comme `ctx` opaque. Le HAL (`hal/audio.c`, pur C)
ne connaît ni `AudioInstrument`, ni le vtable C++, ni même l'existence d'un
objet polymorphe — il stocke juste un `void *` et un pointeur de fonction,
et les repasse tels quels à chaque IRQ (`hal/audio.c:73-74`). C'est
`audio_cb` qui fait le `static_cast` et l'appel virtuel `process()`.

Ce découplage strict C/C++ est ce qui permet à `hal/audio.c` d'être
compilé comme C pur (portable, ABI simple, pas de dépendance à la vtable
C++) tout en laissant toute latitude côté instruments pour utiliser
héritage, virtual dispatch, membres, etc.

### 7.3 Limitations actuelles et piste d'extension

Ce qui n'est **pas** supporté, ni dans le template ni dans un projet
dérivé qui n'y aurait pas travaillé spécifiquement :
- changer d'instrument sans reflasher (pas de bouton "next instrument") ;
- faire tourner deux instruments en parallèle (ex. synth + looper mixés) —
  un seul `process()` est appelé par buffer, un seul objet existe.

Un projet dérivé qui aurait vraiment besoin d'un switch à chaud entre
plusieurs instruments (cas volontairement hors du périmètre du template,
qui vise un instrument par projet) devrait a minima :
1. remplacer le `static Instrument g_instrument` unique par un tableau de
   pointeurs `AudioInstrument*` (un par mode, alloués statiquement — pas de
   `new` dynamique pour rester dans les contraintes IRQ) et un index
   courant modifiable ;
2. protéger le changement d'index contre une course avec l'IRQ DMA (le
   plus simple : ne changer l'index que depuis le main loop, et faire lire
   ce pointeur par `audio_cb` — un `uintptr_t` aligné se lit de façon
   atomique sur Cortex-M33, donc pas besoin de désactiver l'IRQ pour ce cas
   précis) ;
3. définir ce que "changer d'instrument" fait à l'état interne de l'ancien
   (rien, si on le garde en mémoire pour y revenir — mais alors chaque
   instrument occupe sa RAM en permanence, ce qui peut devenir coûteux si
   plusieurs instruments bufferisent de l'audio en SRAM) ;
4. potentiellement appeler un nouveau `reset()`/`init()` sur l'instrument
   entrant pour repartir d'un état propre (silence, position de lecture à
   zéro...).

Rien de tout cela n'est en place — c'est une extension architecturale, à
faire dans un projet dérivé spécifique plutôt que dans le template lui-même.

---

## 8. Build & toolchain

### 8.1 CMake / SDK

- SDK Pico 2.2.0, toolchain `arm-none-eabi-gcc` 14.2.1, board `pico2`
  (RP2350, Cortex-M33).
- `CMAKE_C_STANDARD 11`, `CMAKE_CXX_STANDARD 17` — les `.cpp`
  d'instruments utilisent C++17 (classes, `constexpr`, héritage), les `.c`
  du HAL restent en C11 strict, appelés depuis C++ via `extern "C"`
  (`hal/*.h` sont wrappés `#ifdef __cplusplus extern "C" { ... }`).
- Le bloc "DO NOT EDIT" en tête de `CMakeLists.txt` (lignes 10-23) est
  injecté par l'extension VS Code Raspberry Pi Pico — il pointe vers
  `~/.pico-sdk/cmake/pico-vscode.cmake` pour la résolution du SDK/toolchain
  installés localement. Ne pas le modifier à la main.

### 8.2 ABI float — softfp obligatoire, et rigueur flottante

`target_compile_options` ajoute `-O2` plus trois flags de rigueur flottante :

- `-Wdouble-promotion` — signale tout littéral/expression flottant qui
  promeut silencieusement en `double` (un `0.5` au lieu de `0.5f` coûte
  jusqu'à ~40× plus cher sur cette FPU simple précision). C'est un
  avertissement, pas une erreur bloquante — à corriger dès qu'il apparaît.
- `-fno-math-errno` — autorise le compilateur à réduire `sqrtf()`/etc. à une
  seule instruction FPU au lieu d'un appel libm qui gère `errno`.
- `-fsingle-precision-constant` — les littéraux non suffixés (`0.5`) restent
  `float`, jamais promus en `double` par défaut.

**Ne jamais ajouter** `-mfloat-abi=hard` ni `-mfpu=...` : le SDK Pico est
distribué en `softfp` par défaut pour `pico2`, et mélanger ABI hard/soft casse
le link (`error: uses VFP register arguments`). Les calculs flottants de
`SelfTest` (`sinf`, `cosf`, liés via la lib `m`) passent par les conventions
d'appel softfp sans souci de performance notable pour ce volume de calcul
(une fois par buffer, pas par sample).

### 8.3 Génération PIO

`pico_generate_pio_header(rp2350-LuciboxHAL i2s_duplex.pio)` transforme le
fichier `.pio` en `i2s_duplex.pio.h` dans le dossier de build, inclus par
`hal/audio.c`. Toute modification du `.pio` nécessite une recompilation
complète (pas de hot-reload, évidemment, mais bon à rappeler : l'assembleur
PIO n'est pas revalidé tant qu'on ne relance pas la génération).

### 8.4 Compilation — règle projet

**Rappel explicite (déjà dans `CLAUDE.md`, répété ici car structurant pour
le workflow) : la compilation se fait exclusivement via l'extension VS
Code "Raspberry Pi Pico" → bouton "Compile Project"**, qui invoque Ninja
(`~/.pico-sdk/ninja/v1.12.1/ninja`) avec `-G Ninja`. Un assistant IA
travaillant sur ce dépôt ne doit pas tenter de compiler en ligne de
commande.

Flash : BOOTSEL maintenu + USB, copier `build/rp2350-LuciboxHAL.uf2` sur le
volume `RPI-RP2`.

---

## 9. Pièges connus — référence condensée

Cette section est un résumé technique pour navigation rapide. **La source
de vérité détaillée reste `CLAUDE.md`**, à relire avant toute modification
touchant à l'un de ces points.

| Domaine | Piège | Symptôme si violé |
|---|---|---|
| PIO / I2S | LRCK doit transiter sur front **descendant** de BCLK | silence total |
| Build | Ne jamais forcer `-mfloat-abi=hard`/`-mfpu` | erreur de link VFP |
| ES8311 REG 0x03/0x04 | OSR < 0x10 = invalide | silence (ADC ou DAC) |
| ES8311 REG 0x0D | `PDN_VREF` logique inversée — 1 = activé | silence si 0x0A au lieu de 0x06 |
| ES8311 REG 0x01 | bit 0 `ANACLKDAC_ON` obligatoire (valeur correcte 0x35) | DAC analogique muet |
| Séquencement | I2C avant MCLK (cause EMI **non vérifiée**, cf. §2.6) | corruption I2C, chip non détecté (symptôme observé, cause supposée) |
| Séquencement | CSM_ON après MCLK stable | CSM bloquée < S7 |
| SRAM | Instrument dérivé bufferisant de l'audio : rester sous ~520 KiB total | overflow SRAM / crash |
| PIO sync | Démarrer TX+RX via `pio_enable_sm_mask_in_sync`, jamais séparément | dérive RX/TX de quelques cycles |
| MPR121 | Jamais d'appel I2C depuis l'IRQ DMA (bloquant) | callback audio dépasse son budget temps |
| Perf | Code du chemin chaud (`dma_irq_handler`) doit rester `__not_in_flash_func` — `controls_update()` n'en fait **plus** partie (§4.1) | latence/jitter variable (cache miss flash XIP) |
| Contrôles | Ne jamais appeler `controls_pot_read_raw`/`_norm` depuis `process()` — utiliser `controls_pot_get()`/`controls_button_get()` | zipper noise (bruit ADC brut) / rebond, discipline control-rate cassée |
| Contrôles | Fronts boutons et deltas encodeurs : un seul lecteur (le `process()`) | événements perdus/dupliqués si deux contextes les consomment |

---

## 10. Roadmap / points d'extension

Pistes déjà identifiées dans les headers de code et la documentation,
rassemblées ici pour vue d'ensemble — la plupart concernent des projets
dérivés du template plutôt que le template lui-même :

- **Instruments réels** — synth (contrôle par pots, interpolation
  logarithmique fréquence), sampler (samples en flash via `objcopy`,
  déclenchement par MPR121/pot, pitch shift par accumulateur de phase),
  looper (buffer circulaire SRAM, overdub) : aucun n'existe plus dans ce
  template, à réimplémenter dans chaque projet dérivé selon le besoin,
  en suivant le squelette `SelfTest` (§6.3).
- **MPR121** — implémentation réelle du stub (`controls_mpr121_init/
  update`), lecture des registres `ELE0_ELE7`/`ELE8_ELEPROX`, seuils
  touch/release. Contrainte déjà posée : appelée depuis `controls_update()`
  (main loop, ~50 Hz) uniquement — jamais depuis l'IRQ DMA.
- **Board RP2350B** — fait : `boards/lucibox.h` (flash à confirmer, 4 Mo par défaut).
- **WS2812** — fait : `hal/leds.c/.h`, GPIO 1, 16 LED, PIO1 ; framebuffer écrit
  par `process()`, poussé par `main()` à 50 Hz.
- **Switch instrument à chaud** — hors périmètre du modèle template
  (§7.1) ; voir §7.3 pour les prérequis architecturaux si un projet dérivé
  en a vraiment besoin.
- **MCLK précis** — remplacer le PWM approximatif (+1,7 % d'erreur, §2.7)
  par un `clk_gpout` à diviseur fractionnaire pour un ratio MCLK/Fs exact.
- **Contrôle de volume DAC dynamique** — `es8311_set_dac_volume()` existe
  déjà côté driver mais n'est appelée par aucun instrument actuellement (le
  volume est géré numériquement dans `process()`, pas via le registre
  analogique du codec).
