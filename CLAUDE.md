# rp2350-LuciboxHAL

Firmware audio pour RP2350 + ES8311 : HAL I2S duplex (DAC + ADC), contrôles
par potentiomètres. **Template GitHub** — l'instrument par défaut (`SelfTest`)
est volontairement factice : sweep audio (preuve DAC) + passthrough ADC
(preuve ADC), sans logique musicale. Sert de point de départ ("Use this
template" sur GitHub) pour dériver un projet par instrument réel (synth,
sampler, looper...), chacun remplaçant `SelfTest` par sa propre classe
héritant de `AudioInstrument`. La HAL (`hal/`) ne bouge pas d'un projet
dérivé à l'autre.

---

## ⚠️ Compilation — réservée à l'utilisateur

**Claude ne doit PAS compiler ce projet.** Compilation via VS Code, deux méthodes :

1. **Extension Raspberry Pi Pico** (méthode normale) : icône Raspberry Pi →
   barre latérale gauche → **"Compile Project"**.

2. **Repli CMake Tools** (si le menu de l'extension Pico reste grisé — bug
   connu, pas bloquant) :
   - `Cmd+Shift+P` → **CMake: Select a Kit** → choisir **"Pico"**
   - `Cmd+Shift+P` → **CMake: Configure**
   - `Cmd+Shift+P` → **CMake: Build**

   Même toolchain, même résultat (`build/` généré via Ninja). Le menu grisé
   de l'extension Pico n'empêche pas de builder — ce n'est pas grave si
   "Compile Project" ne fonctionne pas, la méthode 2 marche aussi bien.

Flasher après compilation : maintenir BOOTSEL, brancher USB,
copier `build/rp2350-LuciboxHAL.uf2` sur le lecteur `RPI-RP2`.

---

## Instrument par défaut — SelfTest

Un seul instrument dans ce template : `SelfTest` (`instruments/selftest.hpp/.cpp`).
Aucun paramètre de sélection à la compilation (plus d'`APP_MODE`) — un seul
`.cpp` d'instrument est listé dans `CMakeLists.txt`.

- Sortie DAC : sweep sinusoïdal 440↔470 Hz (LFO 0.1 Hz, LUT + accumulateur
  de phase) — preuve que le chemin DAC fonctionne, même en silence total en entrée.
- Entrée ADC : passthrough du signal capté, mixé au sweep (gain 0.5/0.5,
  clampé) — parler dans le micro / injecter un signal ligne doit s'entendre
  en plus du sweep, preuve que le chemin ADC fonctionne.

### Dériver un nouveau projet depuis ce template

1. GitHub → "Use this template" → nouveau repo.
2. Renommer le projet (`project(...)` et `add_executable(...)` dans
   `CMakeLists.txt`, nom du dossier).
3. Remplacer `instruments/selftest.hpp/.cpp` par ton instrument : nouvelle
   classe héritant de `AudioInstrument` (`init()`, `process()`, `name()`),
   même contrat temps réel (voir `instruments/audio_instrument.hpp`).
4. Mettre à jour la liste de sources dans `CMakeLists.txt` et l'inclusion/
   instanciation dans `main.cpp`.
5. La HAL (`hal/`) et `i2s_duplex.pio` ne changent en principe pas — ce sont
   les fondations communes à tous les projets dérivés.

---

## Brochage GPIO

| GPIO | Signal | Interface |
|------|--------|-----------|
| 6 | SDA | I2C1 → ES8311 config |
| 7 | SCL | I2C1 → ES8311 config |
| 10 | BCLK | I2S bit clock (PIO0 SM0 side-set bit 0) |
| 11 | LRCK | I2S word select (PIO0 SM0 side-set bit 1, = BCLK+1) |
| 12 | DACDAT | I2S TX : RP2350 → ES8311 DI |
| 13 | ADCDAT | I2S RX : ES8311 DO → RP2350 |
| 14 | MCLK | PWM 12.5 MHz (150 MHz / 12) |
| 26-29 | Pot 0-3 | ADC interne ch0-3 → non utilisés par `SelfTest`, réservés aux instruments dérivés |

---

## Architecture

```
ES8311 ADC → ADCDAT (GPIO13) → PIO0 SM1 → DMA RX → rx_buf[]
                                                         ↓
                                                  audio_cb() → instrument.process()
                                                         ↓
tx_buf[] → DMA TX → PIO0 SM0 → DACDAT (GPIO12) → ES8311 DAC
```

### Ordre d'init (CRITIQUE)

1. **I2C** — avant MCLK (ordre conservé par précaution ; la cause "EMI" n'est qu'une hypothèse non testée — voir §Pièges connus)
2. **ES8311 registres** — DAC + ADC, sans CSM_ON
3. **Controls** — ADC pots (GPIO 26-29)
4. **HAL audio** — PIO + DMA (non démarrés)
5. **Instrument** — init() de l'instrument (`SelfTest` par défaut)
6. **MCLK** — PWM GPIO 14, activé après I2C
7. **ES8311 CSM_ON** — REG 0x00 = 0x80, après MCLK
8. **audio_hal_start()** — DMA + PIO activés simultanément

### Callback audio

```c
// Appelée depuis IRQ DMA toutes les ~5.3 ms (256 frames @ 48 kHz)
// → instrument.process(rx_buf[just_done], tx_buf[just_done], 256)
```

Format buffers vu par l'instrument : `int16_t[256]`, **mono**. Le HAL convertit vers/depuis
le fil I2S stéréo — voir §Mono vs stéréo ci-dessous.

### Mono vs stéréo (ES8311)

Le codec ES8311 est **mono** (un seul DAC, un seul ADC), mais le fil I2S Philips
transporte toujours 2 slots de 16 bits par frame — c'est une contrainte du protocole,
pas un choix de nombre de canaux (voir `i2s_duplex.pio`, timing CRITIQUE, ne pas
toucher). La HAL absorbe entièrement cette conversion : `audio_process_fn` et
`AudioInstrument::process()` ne voient jamais que du **mono `int16_t`**, jamais de L/R.

La conversion mono ↔ stéréo (frame `int32_t` `[L:16][R:16]`) vit dans `hal/audio.c`
(`unpack_mono_rx()` / `pack_mono_tx()`), pilotée par deux defines dans `hal/audio.h`,
résolus **à la compilation** (`#if`, coût runtime nul) :

```c
#define AUDIO_RX_SLOT   AUDIO_SLOT_LEFT    // slot I2S lu pour l'entrée ADC
#define AUDIO_TX_SLOT   AUDIO_SLOT_BOTH    // slot(s) I2S écrit(s) pour la sortie DAC
```

Valeurs possibles : `AUDIO_SLOT_LEFT`, `AUDIO_SLOT_RIGHT` (les deux directions),
`AUDIO_SLOT_BOTH` (TX uniquement — duplique l'échantillon mono sur L et R).
Par défaut : lecture sur L, écriture dupliquée sur L+R. À redéfinir via
`target_compile_definitions` dans `CMakeLists.txt` si un futur câblage du codec
diffère (ex. signal mono câblé uniquement côté R).

⚠️ Aucun instrument ne doit manipuler L/R lui-même — ce choix appartient à la HAL.

### Code en SRAM + flags de compilation

`dma_irq_handler()` (`hal/audio.c`) est marqué `__not_in_flash_func()` : il tourne
depuis la SRAM, pas depuis le cache flash XIP — élimine tout risque de latence
variable due à un cache miss dans le chemin le plus critique du système (appelé
~188 fois/s, budget ~5,3 ms). `controls_update()` et les lectures ADC brutes
qu'elle appelle (`controls_pot_read_raw`/`_norm`, dans `hal/controls.c`) sont
marquées pareil, car elles sont désormais invoquées à chaque bloc **depuis
`dma_irq_handler()`** — donc sur le même chemin chaud (voir ci-dessous).

`CMakeLists.txt` active `-Wdouble-promotion -fno-math-errno
-fsingle-precision-constant` sur toute la cible :
- `-Wdouble-promotion` signale tout littéral/expression flottant qui promeut
  silencieusement en `double` (un `0.5` au lieu de `0.5f` coûte ~40× plus cher
  sur cette FPU). C'est un avertissement, pas une erreur — à corriger au fil de
  l'eau si le build en affiche.
- `-fno-math-errno` autorise le compilateur à réduire `sqrtf()` (et consorts) à
  une seule instruction FPU au lieu d'un appel libm qui gère `errno`.
- `-fsingle-precision-constant` fait que les littéraux non suffixés restent en
  `float`, jamais promus en `double` par défaut.

### Lecture des potentiomètres (control rate mécanique)

Les instruments ne doivent **jamais** appeler `controls_pot_read_raw()` /
`controls_pot_read_norm()` depuis `process()` — ces fonctions déclenchent une
vraie conversion ADC, non filtrée, à chaque appel. À la place :

- `controls_update()` lit les `CONTROLS_NUM_POTS` canaux et applique un lissage
  un pôle (`CONTROLS_POT_SMOOTH_COEFF`, ~16 ms de constante de temps). Appelée
  **automatiquement une fois par bloc** par `dma_irq_handler()`, avant
  `instrument.process()` — ce n'est plus une convention à respecter, c'est
  mécanique : l'instrument n'a plus la main sur le moment où l'ADC est lu.
- `controls_pot_get(ch)` renvoie la valeur mise en cache/lissée, 0.0..1.0, en
  O(1), sans accès ADC. C'est la **seule** fonction à utiliser dans
  `process()`.

---

## Structure des fichiers

```
rp2350-LuciboxHAL/
├── main.cpp                        # Entry point, instanciation de l'instrument
├── i2s_duplex.pio                  # PIO TX (validé) + PIO RX (synchronisé)
├── CMakeLists.txt
├── hal/
│   ├── audio.h/.c                  # MCLK PWM, PIO I2S duplex, DMA double-buffer
│   ├── es8311.h/.c                 # Driver codec ES8311 (DAC + ADC)
│   └── controls.h/.c               # ADC pots + MPR121 stub
└── instruments/
    ├── audio_instrument.hpp        # Classe de base (pure virtual)
    └── selftest.hpp/.cpp           # Instrument fake : sweep DAC + passthrough ADC
```

---

## Registres ES8311 clés

Tous les registres de `rp2350-BasicSine` s'appliquent, plus :

| Registre | Valeur | Raison |
|----------|--------|--------|
| 0x03 CLK3 | 0x10 | ADC OSR min valide (mêmes contraintes que 0x04 DAC OSR) |
| 0x0E SYS2 | 0x00 | Active PGA + modulateur ADC |
| 0x14 ADC_ANA | 0x10 | LINSEL=1 : entrée ligne MIC1P/MIC1N activée |
| 0x16 MIC_GAIN | 0x08 | bits[4:2]=010 : PGA +6 dB |
| 0x1C ADC_CTRL | 0x00 | ADC activé (mode normal) |

---

## Toolchain & SDK

- SDK Pico : 2.2.0
- Toolchain : arm-none-eabi-gcc 14.2.1 (`~/.pico-sdk/toolchain/14_2_Rel1/`)
- Board : `pico2` (RP2350, Cortex-M33)
- Float ABI : **softfp** (défaut SDK pico2)
- Stdio : USB CDC uniquement

---

## Pièges connus

### PIO LRCK timing (CRITIQUE)
LRCK doit transiter sur le flanc **DESCENDANT** de BCLK — instruction `out pins,1 side 0b10`
(dernier bit L) et `out pins,1 side 0b00` (dernier bit R).
Transiter sur le flanc MONTANT = violation setup ES8311 = **silence total**.
**Ne pas modifier ce timing sans relire ce document.**

### ABI float
Ne **JAMAIS** ajouter `-mfloat-abi=hard` ni `-mfpu=...` dans `target_compile_options`.
Cause ABI mismatch au link avec les libs SDK softfp.
Symptôme : `error: uses VFP register arguments`.

### ES8311 OSR (registres 0x03 et 0x04)
Valeurs 0x00..0x0F = **silence total**. Minimum valide = **0x10**.
S'applique au DAC (REG 0x04) ET à l'ADC (REG 0x03).

### ES8311 SYS1 (REG 0x0D) — logique inversée
`PDN_VREF bit2 = 1` = ACTIVÉ (pas désactivé). 0x06 = VREF actif. 0x0A = silence.

### ES8311 CLK1 (REG 0x01) — ANACLKDAC_ON
bit 0 = 1 OBLIGATOIRE. Sans lui, le DAC analogique est silencieux.
Valeur correcte : 0x35.

### ⚠️ HYPOTHÈSE NON VALIDÉE — "EMI MCLK sur I2C"
**Ceci est une supposition, pas un fait établi.** Aucun test EMI n'a jamais
été réalisé (pas d'oscillo, pas d'analyseur de spectre, pas d'essai
contrôlé en inversant l'ordre). L'explication "le PWM MCLK génère des
perturbations EMI qui corrompent l'I2C" vient de `rp2350-BasicSine` et a
été recopiée telle quelle — elle a pu se figer en "certitude" par simple
répétition sans jamais avoir été vérifiée. La vraie cause d'un éventuel
échec d'init pourrait tout aussi bien être un problème de timing CSM, une
race condition logicielle, ou autre chose sans rapport avec l'EMI.

En pratique : l'ordre I2C → MCLK (étape 1 avant étape 6) est conservé par
précaution, pas parce que la cause EMI est démontrée.

**Un protocole de test serait bienvenu** : inverser l'ordre (activer MCLK
avant l'init I2C) et observer si l'échec est reproductible (chip ID non lu,
timeout I2C...), idéalement avec une mesure EMI réelle (oscillo sur SDA/SCL
pendant que MCLK tourne) pour confirmer ou infirmer l'hypothèse.

### CSM_ON après MCLK
REG 0x00 = 0x80 uniquement après MCLK stable. La CSM transite S0→S7 uniquement si MCLK présent.

### SRAM (instruments dérivés)
RP2350 total = 520 KB. Un buffer audio en SRAM à 48 kHz mono ≈ 192 KB/s —
un looper à 2s occupe déjà 384 KB. À surveiller pour tout instrument dérivé
qui bufferise de l'audio (looper, sampler avec RAM scratch, etc.).

### PIO RX synchronisation
Le SM RX (SM1) partage le même `clkdiv` que SM TX (SM0).
Démarrer avec `pio_enable_sm_mask_in_sync((1<<SM_TX)|(1<<SM_RX))` — c'est ce que `audio_hal_start()` fait.

### MPR121
Addr I2C 0x5A, bus I2C1 (partagé avec ES8311 addr 0x18) → compatible.
Ne pas appeler depuis IRQ DMA — I2C est bloquant. Utiliser le main loop.
