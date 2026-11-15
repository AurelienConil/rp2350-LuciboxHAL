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
| 18-25 | Encodeur 0-3 | 4 encodeurs quadrature (A,B par paire : 18/19, 20/21, 22/23, 24/25), sans push — décodés par timer 1 kHz |
| 26-31 | Bouton simple 0-5 | GPIO direct, pull-up, actif bas (hypothèse : D26..D31 contigus) |
| 32/34/36/38 | Bouton lumineux 0-3 | switch (indices boutons 6-9), pull-up, actif bas |
| 33/35/37/39 | LED bouton lumineux 0-3 | sortie, `controls_led_set(n, on)` |
| 40-47 | Pot 0-7 (A0-A7) | ADC interne ch0-7 — **RP2350B uniquement** (voir §Pièges : board) |
| 1 | WS2812 data (16 LED) | PIO1, `hal/leds.h` : `leds_set()` (framebuffer, OK depuis `process()`), `leds_show()` appelé par `main()` |

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
3. **Controls** — pots (ADC GPIO 40-47), boutons, LEDs, encodeurs (timer 1 kHz)
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
~188 fois/s, budget ~5,3 ms). `controls_update()` et les lectures capteurs
qu'elle appelle (`hal/controls.c`) ne sont **plus** sur ce chemin chaud — elles
tournent depuis la boucle `main()`, à ~50 Hz, totalement découplées du rythme
audio (voir §"Lecture des capteurs" ci-dessous) — donc pas besoin de
`__not_in_flash_func` dessus.

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

### Lecture des capteurs (control rate, hors IRQ audio)

Pots, boutons et (à terme) capteurs capacitifs I2C sont scannés depuis une
**seule tâche dans la boucle `main()`**, à `CONTROLS_UPDATE_PERIOD_MS` (~50 Hz,
`hal/controls.h`) — **jamais** depuis l'IRQ audio. C'est ce découplage qui rend
sûr d'y inclure demain de l'I2C bloquant (MPR121) à côté des pots : l'IRQ audio
ne touche plus jamais un capteur, quel qu'il soit.

Les instruments ne doivent **jamais** appeler `controls_pot_read_raw()` /
`controls_pot_read_norm()` depuis `process()` — ces fonctions déclenchent une
vraie conversion ADC, non filtrée, à chaque appel. À la place :

- `controls_update()` scanne pots (`CONTROLS_NUM_POTS = 8`), boutons
  (`CONTROLS_NUM_BUTTONS = 10` : 6 simples + 4 lumineux, indices
  `CONTROLS_BTN_SIMPLE(n)` / `CONTROLS_BTN_LIT(n)`) et MPR121. Appelée
  **automatiquement à chaque tick de `main()`**, jamais par l'instrument.
  Brochage : tables en tête de `hal/controls.c`.
- Encodeurs (4) : décodés par un timer répétitif 1 kHz (un scan 50 Hz perdrait
  des pas), pas par `controls_update()`.
- Pots : lissage un pôle, constante de temps calculée à l'init depuis
  `CONTROLS_POT_SMOOTH_TIME_MS` (pas un coefficient magique — reste correct si
  `CONTROLS_UPDATE_PERIOD_MS` change).
- Boutons : anti-rebond par confirmation sur `CONTROLS_BUTTON_DEBOUNCE_SAMPLES`
  lectures consécutives identiques (~40 ms par défaut à 50 Hz). Pull-up
  interne, appui = niveau bas — masqué par l'API (`controls_button_get()`
  renvoie `true` = appuyé, quel que soit le câblage).
- API utilisable dans `process()` (toutes O(1), sans accès matériel) :
  `controls_pot_get(ch)` (0..1 lissé), `controls_button_get(i)` (niveau),
  `controls_button_pressed(i)` / `_released(i)` (front, consommé à la lecture),
  `controls_encoder_delta(i)` (crans depuis le dernier appel, signé),
  `controls_led_set(n, on)`. Fronts et deltas : **un seul lecteur** (le
  `process()` de l'instrument) — l'état "déjà lu" est côté lecteur.

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
│   └── controls.h/.c               # pots, boutons, LEDs, encodeurs + MPR121 stub
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

### Board RP2350B (GPIO > 31)
Le PCB utilise les GPIO 32-47 : la cible est un **RP2350B** (80 broches).
`CMakeLists.txt` utilise la board custom `boards/lucibox.h` (`PICO_RP2350A 0`)
au lieu de `pico2`. ⚠ `PICO_FLASH_SIZE_BYTES` y est copié de pico2 (4 Mo) — à
régler sur la flash réelle. Après ce changement, **reconfigurer CMake**
(supprimer `build/` ou "CMake: Delete Cache and Reconfigure").

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
