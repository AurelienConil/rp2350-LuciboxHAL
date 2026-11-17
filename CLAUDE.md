# rp2350-LuciboxHAL

Firmware audio pour RP2350 + ES8311 : HAL I2S duplex (DAC + ADC), contrôles
par potentiomètres. **Template GitHub** — l'instrument par défaut (`Instrument`, auto-test)
est volontairement factice : sweep audio (preuve DAC) + passthrough ADC
(preuve ADC), sans logique musicale. Sert de point de départ ("Use this
template" sur GitHub) pour dériver un projet par instrument réel (synth,
sampler, looper...), chacun réécrivant `Instrument` avec sa propre logique
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

## Instrument par défaut — `Instrument` (démo SelfTest)

Un seul instrument par firmware : la classe `Instrument`
(`instruments/instrument.hpp/.cpp`). Nom de classe et de fichiers **fixes** :
`main.cpp` et `CMakeLists.txt` n'ont jamais à changer. Le contenu fourni est un
auto-test factice (ex-« SelfTest »).

- Sortie DAC : sweep sinusoïdal 440↔470 Hz (LFO 0.1 Hz, LUT + accumulateur
  de phase) — preuve que le chemin DAC fonctionne, même en silence total en entrée.
- Entrée ADC : passthrough du signal capté, mixé au sweep (gain 0.5/0.5,
  clampé) — parler dans le micro / injecter un signal ligne doit s'entendre
  en plus du sweep, preuve que le chemin ADC fonctionne.

### Dériver un nouveau projet depuis ce template

1. GitHub → "Use this template" → nouveau repo.
2. Renommer le projet (`project(...)` et `add_executable(...)` dans
   `CMakeLists.txt`, nom du dossier).
3. Réécrire `instruments/instrument.hpp/.cpp` (seuls fichiers à modifier) :
   garder le nom de classe `Instrument`, héritant de `AudioInstrument`
   (`init()`, `process()`, `name()`, plus les hooks contrôle optionnels
   `on_button()`, `on_encoder()`, `on_tick()`), voir
   `instruments/audio_instrument.hpp`. Ajouter d'autres `.cpp` d'instrument
   si besoin dans `CMakeLists.txt`.
4. La HAL (`hal/`) et `i2s_duplex.pio` ne changent en principe pas — ce sont
   les fondations communes à tous les projets dérivés.

### Deux contextes d'exécution par instrument

| Contexte | Méthodes | Cadence | Règles |
|----------|----------|---------|--------|
| AUDIO (IRQ DMA) | `process()` | ~188 Hz | temps réel strict (voir plus bas) |
| CONTROL (boucle `main()`) | `on_button()`, `on_encoder()`, `on_tick()` | ~50 Hz | printf/LEDs OK, pas d'IRQ ; main() appelle `leds_show()` ensuite |

`main()` est l'**unique lecteur** des fronts de boutons et deltas d'encodeurs
(`dispatch_controls()`) : `process()` ne doit plus appeler
`controls_button_pressed/_released` ni `controls_encoder_delta`. Les données
passent du contrôle vers l'audio via `std::atomic` (paramètres indépendants)
ou une file SPSC lock-free — jamais de mutex. L'horloge musicale (pas, tempo)
vit dans `process()` (compteur d'échantillons), pas dans `on_tick()` (jitter
~20 ms).

---

## Brochage GPIO

| GPIO | Signal | Interface |
|------|--------|-----------|
| 16 | SDA | I2C0 (bus partagé, `hal/i2c_bus.h`) → ES8311, TPA6130A2, CY8CMBR3116 |
| 17 | SCL | I2C0 (bus partagé) |
| 6 | HI (IRQ tactile) | entrée, CY8CMBR3116 pin 23 (HI/BUZ/GPO7), actif bas |
| 10 | BCLK | I2S bit clock (PIO0 SM0 side-set bit 0) |
| 11 | LRCK | I2S word select (PIO0 SM0 side-set bit 1, = BCLK+1) |
| 9 | DACDAT | I2S TX : RP2350 → ES8311 DSDIN (hors du groupe BCLK/LRCK : `out pins` est indépendant du side-set) |
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
tx_buf[] → DMA TX → PIO0 SM0 → DACDAT (GPIO9) → ES8311 DAC
```

### Ordre d'init (CRITIQUE)

1. **I2C** (I2C0, GPIO 16/17) — avant MCLK (ordre conservé par précaution ; la cause "EMI" n'est qu'une hypothèse non testée — voir §Pièges connus)
2. **ES8311 registres** — DAC + ADC, sans CSM_ON
3. **Controls** — pots (ADC GPIO 40-47), boutons, LEDs, encodeurs (timer 1 kHz)
4. **HAL audio** — PIO + DMA (non démarrés)
5. **Instrument** — init() de l'instrument (`Instrument`)
6. **MCLK** — PWM GPIO 14, activé après I2C
7. **ES8311 CSM_ON** — REG 0x00 = 0x80, après MCLK
8. **audio_hal_start()** — DMA + PIO activés simultanément

### Callback audio

```c
// Appelée depuis IRQ DMA toutes les ~5.24 ms (256 frames @ 48828 Hz)
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

Pots, boutons et capteurs capacitifs I2C (CY8CMBR3116) sont scannés depuis une
**seule tâche dans la boucle `main()`**, à `CONTROLS_UPDATE_PERIOD_MS` (~50 Hz,
`hal/controls.h`) — **jamais** depuis l'IRQ audio. C'est ce découplage qui rend
sûr d'y faire de l'I2C bloquant (touch) à côté des pots : l'IRQ audio
ne touche plus jamais un capteur, quel qu'il soit.

Les instruments ne doivent **jamais** appeler `controls_pot_read_raw()` /
`controls_pot_read_norm()` depuis `process()` — ces fonctions déclenchent une
vraie conversion ADC, non filtrée, à chaque appel. À la place :

- `controls_update()` scanne pots (`CONTROLS_NUM_POTS = 8`), boutons
  (`CONTROLS_NUM_BUTTONS = 26` : 6 simples + 4 lumineux + 16 pads tactiles,
  indices `CONTROLS_BTN_SIMPLE(n)` / `CONTROLS_BTN_LIT(n)` /
  `CONTROLS_BTN_TOUCH(n)`). Appelée
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
- API (toutes O(1), sans accès matériel) : `controls_pot_get(ch)` (0..1 lissé)
  et `controls_button_get(i)` (niveau) sont utilisables dans `process()` ;
  `controls_button_pressed(i)` / `_released(i)` (front, consommé à la lecture)
  et `controls_encoder_delta(i)` (crans depuis le dernier appel, signé) sont
  lus **uniquement par `dispatch_controls()` dans `main()`**, qui les livre à
  l'instrument via `on_button()` / `on_encoder()`. `controls_led_set(n, on)`
  utilisable des deux côtés. Fronts et deltas : **un seul lecteur** — l'état
  "déjà lu" est côté lecteur.

---

## Bring-up (premier flash d'un PCB) — lire la console USB

Le firmware est volontairement bavard au boot (console série USB, 115200 sans
importance). Ordre et ce qu'il faut y voir :
1. `[I2C]` : niveaux de repos de SDA/SCL (doivent valoir 1/1), récupération de
   bus si SDA bloqué, puis **scan 0x08..0x77** avec tableau ES8311 (0x18) /
   TPA6130A2 (0x60) / CY8CMBR3116 (0x37) : `found` ou `MISSING` + piste de câblage.
2. `[ES8311]` : ID puce. Absent = **non fatal** (le firmware continue sans audio
   pour pouvoir tester le reste).
3. `[TOUCH]` : famille/ID, (re)configuration éventuelle, auto-test du capteur
   (pads court-circuités, CMOD), niveau de repos de l'IRQ (GPIO 6), puis un log à
   chaque changement de pads avec compteur d'IRQ.
4. `[CONTROLS]` : valeurs brutes des 8 pots, niveaux des boutons/encodeurs au boot.
5. `[TPA]` : ampli casque TPA6130A2, driver `hal/tpa6130a2.h/.c` (`tpa_init()`
   appelé dans `main.cpp`). Au reset il est désactivé et muté (REG1=0x00,
   REG2=0xC0) : le driver lit la version (REG4, attendu 0x2), règle le volume
   `TPA_VOLUME` (code 0..63, courbe NON linéaire, datasheet SLOS488F Table 2 :
   0x08 = -36.5 dB) tout en restant muté, active L+R puis démute (sans pop).
   API : `tpa_set_volume()/_db()`, `tpa_mute()`, `tpa_shutdown()`,
   `tpa_set_mode()`, `tpa_set_hiz()`, `tpa_thermal_fault()`, `tpa_dump()`.
   REG5..8 (tests TI) jamais écrits. Sa broche SD doit être à 1.
6. `BRING-UP SUMMARY` : une ligne par composant.

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
│   └── controls.h/.c               # pots, boutons, LEDs, encodeurs, pads tactiles
│   ├── tpa6130a2.h/.c              # driver ampli casque TPA6130A2 (I2C 0x60)
│   ├── touch.h/.c                  # driver CY8CMBR3116 (16 pads, I2C 0x37, IRQ GPIO 6)
│   └── i2c_bus.h                   # bus I2C partagé (I2C0, GPIO 16/17)
└── instruments/
    ├── audio_instrument.hpp        # Classe de base : process() audio + hooks contrôle
    └── instrument.hpp/.cpp         # Instrument (à réécrire) ; fourni = fake : sweep DAC + passthrough ADC
                                    # + démo hooks (encodeur 0 = pitch, boutons, LED)
```

---

## Flash 16 Mo + PSRAM 8 Mo — état et test mis de côté

- **Flash : validée** sur le PCB (JEDEC `BA 40 18`, SFDP 128 Mbit = 16 Mo, erase/program/verify OK
  sur les 5 fins de tranche). `boards/lucibox.h` : `PICO_FLASH_SIZE_BYTES` = 16 Mo.
- **PSRAM : non utilisable en l'état.** Monté : APS6404L-**SQN**-SN = version **1,8 V** (VDD 1,62–1,98 V,
  abs. max 2,45 V) alimentée en 3,3 V → hors spécification ; crash/déconnexion USB à la 1re lecture
  (2026-11-17). À remplacer par **APS6404L-3SQR-SN** (3,3 V, même SOP-8, même brochage) + corriger la BOM.
  Constats utiles : CS1 = GPIO 0, ID `0D 5D`, EID `4B 32 99 B5 83 10`, 8 Mo (64 Mbit).
- Le test mémoire (`memtest.h/.c`) est **gardé de côté, non compilé**, dans `extras/memtest/`
  (README : comment le réactiver). Rien de PSRAM/flash-test n'est dans le firmware.

---

## Registres ES8311 clés

Tous les registres de `rp2350-BasicSine` s'appliquent, plus :

| Registre | Valeur | Raison |
|----------|--------|--------|
| 0x01 CLK1 | **0x3F** | bit 3 = `CLKADC_ON` (User Guide §8.5). 0x35 = DAC seul → ADC non cadencé → zéros exacts |
| 0x03 CLK3 | 0x10 | ADC OSR min valide (mêmes contraintes que 0x04 DAC OSR) |
| 0x0E SYS2 | 0x02 | PGA (bit 6) + modulateur ADC (bit 5) activés (défaut guide = 0x6A = éteints) |
| 0x14 ADC_ANA | 0x10 \| `ES8311_PGA_GAIN_INIT` | LINSEL=1 (MIC1P/MIC1N) ; PGAGAIN bits[3:0] = 0..10 → 0..+30 dB par 3 dB (défaut projet : 0 dB) |
| 0x16 ADC_SCALE | `ES8311_ADC_SCALE_INIT` (3) | bits[2:0] : 0..7 = 0..+42 dB par 6 dB, **nominal du guide = 4** (+24 dB) ; projet : 3 (−6 dB) |
| **0x17 ADC_VOL** | **0xBF** | ⚠ **défaut 0x00 = −95.5 dB = ADC muet.** À écrire obligatoirement (0xBF = 0 dB) |
| 0x1C ADC_CTRL | 0x6A | EQ de l'ADC contourné + filtre passe-haut DC dynamique |
| 0x1B | 0x0A | coefficient du HPF ADC (valeur du driver Espressif, **non documentée dans le guide**) |
| 0x10, 0x11 | 0x1F, 0x7F | idem : valeurs du driver Espressif, **absentes du guide** (retirer en premier si doute) |

Source de référence : **`ES8311.user.Guide.pdf` (Rev 1.11)** — contient la carte des
registres (la datasheet courte `ES8311.PDF` Rev 5.0 ne l'a pas). Les valeurs marquées
« Espressif » viennent du driver `esp-adf` / `esp_codec_dev`, de mémoire, non vérifiées.

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
⚠ Piège vécu : avec un cache resté sur `PICO_BOARD=pico2`, `NUM_BANK0_GPIOS=30` et
tout GPIO ≥ 32 est faux (`gpio_get` renvoie 0 : boutons lumineux « appuyés » au
boot, pots ADC figés). Vérifier `PICO_BOARD:STRING=lucibox` dans
`build/CMakeCache.txt`.

### ABI float
Ne **JAMAIS** ajouter `-mfloat-abi=hard` ni `-mfpu=...` dans `target_compile_options`.
Cause ABI mismatch au link avec les libs SDK softfp.
Symptôme : `error: uses VFP register arguments`.

### Niveau de sortie casque
Chaîne : DAC ES8311 (0 dB numérique, `DAC_VOL` = 0xBF) → TPA6130A2 → casque. Pleine échelle
du DAC = AVDD/3.3 Vrms (~1 Vrms à 0 dBFS si AVDD = 3.3 V ; AVDD **non vérifié** sur ce PCB, ni le
câblage OUTP/OUTN → entrées asymétriques du TPA). Réglages : `TPA_VOLUME` (`hal/tpa6130a2.h`,
ou `-DTPA_VOLUME=0x..`), table `VOLUME_DB[]` dans `hal/tpa6130a2.c` (non linéaire) :
0x05 = −43.9, 0x08 = −36.5, 0x10 = −24.7, 0x14 = −20.5 (**valeur actuelle**), 0x17 = −17.8,
0x28 = −6.1, 0x35 = +0.1 dB (unité), 0x3F = +4.0 dB. Le gain maximal du TPA est donc +4 dB :
il ne rattrape pas un signal d'entrée trop faible.
⚠ Pleine échelle numérique + TPA haut = très fort au casque : monter par petits pas.

### ES8311 OSR (registres 0x03 et 0x04)
Valeurs 0x00..0x0F = **silence total**. Minimum valide = **0x10**.
S'applique au DAC (REG 0x04) ET à l'ADC (REG 0x03).

### ES8311 SYS1 (REG 0x0D) — logique inversée
`PDN_VREF bit2 = 1` = ACTIVÉ (pas désactivé). 0x06 = VREF actif. 0x0A = silence.

### ES8311 CLK1 (REG 0x01) — horloges DAC *et* ADC
bit 0 (ANACLKDAC_ON, d'après le driver Espressif) = 1 OBLIGATOIRE : sans lui, le DAC analogique est silencieux.
bit 3 = `CLKADC_ON` (User Guide §8.5) = 1 OBLIGATOIRE pour l'ADC : sans lui l'ADC sort des **zéros exacts**.
Valeur correcte : **0x3F**. (L'ancien 0x35 était validé pour la sortie seule.)

### ES8311 — rapport MCLK/LRCK : multiple de 16 (User Guide §8.5)
MCLK = 12.5 MHz (PWM 150/12). À 48000 Hz, MCLK/Fs = **260.4**, rapport **invalide** pour l'ADC
(attendu : 256, 272...). Symptôme : ADC qui délire (rafales à pleine échelle puis silence parfait,
valeurs en multiples de 1024). Correctif : `AUDIO_SAMPLE_RATE = 48828` (`hal/audio.h`) →
12.5e6/48828.125 = **256 exact**, `clkdiv` PIO = 48.0 (entier). Le sweep tombe alors à 440 Hz pile.

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

### CY8CMBR3116 (touch, `hal/touch.c`)
Addr I2C 0x37, bus I2C0 partagé (ES8311 0x18). IRQ = broche `HI` du capteur →
GPIO 6 (front descendant, impulsion 200–700 µs : l'ISR ne fait que lever un
drapeau, la lecture I2C se fait dans `controls_update()`).
- Config d'usine ≠ ce PCB : seuls CS0..7 actifs, `HI` sur la pin 9, pin 23 =
  GPO7. `touch_init()` écrit `SENSOR_EN=0xFFFF` et `SPO_CFG=0x14` (SPO1=CS15,
  SPO0=HI sur pin 23) **une seule fois** (lit d'abord ; ne touche la flash du
  capteur — 100 k cycles — que si la config diffère), CRC calculé par le capteur
  (`CTRL_CMD 3`), sauvegarde (`CTRL_CMD 2`, ~220 ms), reset (`CTRL_CMD 255`).
- Le capteur NACK la 1re transaction après sommeil : toutes les lectures/écritures
  retentent jusqu'à l'ACK (~2 ms d'écart). Boot du capteur jusqu'à ~900 ms.
- Pad n = CSn = bit n de `BUTTON_STAT` (0xAA) → bouton `CONTROLS_BTN_TOUCH(n)`.
- Numérotation : pads 0..15 (`tactile 0` = 1er capteur CS0, `tactile 2` = 3e).
  Indice de bouton dans l'API = 10 + n (`controls_button_get(12)` = `tactile 2`).
  Le log `[Instrument]` affiche `tactile n` (n = numéro de pad, = bit n du
  `[TOUCH] pads 0x....`) pour les pads, et `button N` (0..9) pour les simples/lumineux.
  Les encodeurs (GPIO 18..25) n'ont pas d'indice de bouton (hook `on_encoder()`).
- ⚠ Matériel : le pad CS6 (pin 18) sert aussi de reset au boot, il ne doit pas
  être à la masse à la mise sous tension.
Ne pas appeler depuis IRQ DMA — I2C est bloquant. Utiliser le main loop.

---

## Journal — mise au point de l'entrée micro (branche `add-mic-input`)

État au 2026-11-17. Voir aussi §« Registres ES8311 clés » et §« Pièges connus ».

### Câblage et source
- Mini-jack 4 pôles : pôle micro → **MIC1P** via un condensateur 1 µF ; **MIC1N** → AGND via 1 µF
  (entrée asymétrique, couplée en AC ; l'ES8311 n'a **pas** de bias micro).
- Source : préampli **SmartRig** (micro XLR → smartphone), gain réglé sur l'appareil (très bas).
  Le guide ES8311 précise que l'entrée est une entrée micro différentielle, pleine échelle ≈ 1 Vrms
  à PGA 0 dB, impédance d'entrée 6 kΩ, « pas recommandée pour une entrée ligne ».

### Réglages ADC actuels (valeurs de départ, tous surchargeables par `-D...`)
`ES8311_PGA_GAIN_INIT = 0` (0 dB), `ES8311_ADC_SCALE_INIT = 3` (−6 dB), `ADC_VOL = 0xBF`.
Avec un micro sensible et le SmartRig au minimum, les plosives montent à 50–70 % de pleine échelle
alors que la voix normale est à −40/−45 dBFS (**écart 40–50 dB**).

### Bugs trouvés (dans l'ordre), tous liés à l'ADC
1. `ADC_VOL` (REG 0x17) jamais écrit → défaut −95.5 dB.
2. `CLKADC_ON` (REG 0x01 bit 3) absent → ADC non cadencé → `OR = 0` sur ADCDAT (la ligne était
   pilotée à 0 par le codec ; un pull-up GPIO n'y changeait rien → le codec émettait des zéros).
3. MCLK/Fs = 260.4 (invalide) → ADC erratique → `AUDIO_SAMPLE_RATE` passé à 48828.
4. `es8311_set_mic_gain()` écrivait dans le mauvais registre (0x16 au lieu de 0x14[3:0]) : corrigé.
5. Gains trop forts : PGA +30 dB + `ADC_SCALE` +24 dB saturaient l'ADC (tout à ±32768) avec une source
   de niveau téléphone → PGA 0 dB.

### Ce qui a été essayé et **ne doit pas être refait tel quel**
- `irq_set_priority(DMA_IRQ_0, 0)` (priorité max de l'IRQ audio) : provoque un **énorme bruit de fond**
  (confirmé sur le PCB). Cause non élucidée. Retiré.
- Écrêtage dur de sortie à ±12000 (« protection des oreilles ») : crée des harmoniques audibles sur
  une rafale de grave → « saturation » à chaque plosive. Retiré.
- Limiteur sans anticipation : ne tient pas sur un front de plosive. Retiré.
- Limiteur à anticipation (ligne à retard 96 échantillons, min glissant + moyenne glissante) livré
  avec voix à +10.5 dB : l'utilisateur a rapporté qu'il « fait exploser le micro ». **Cause non
  identifiée** (pas de bug évident à la relecture ; hypothèse : le gain voix +10.5 dB par défaut,
  ajouté en même temps, a augmenté de ~10 dB le gain de la boucle micro → casque → micro = larsen).
  Retiré. À refaire séparément, voix à 0 dB, casque éloigné des oreilles pour le premier test.
- Gain numérique voix élevé (+10.5 dB) : les plosives (~20000) × 3.35 dépassent la pleine échelle
  → saturation int16. Préférer le gain analogique (SmartRig, TPA) au gain numérique.

### Observations sur les plosives
Un B/P près du micro produit un front brutal puis une décroissance lente. Un passe-haut le laisse
passer en grande partie (réponse à un échelon) : sortie ≈ 89 % de l'entrée avec un 4ᵉ ordre à 200 Hz.
Un passe-haut (réglé ici à 100 Hz, 4ᵉ ordre, `HPF_HZ`/`HPF_ENABLED` dans `instrument.hpp`) aide pour
le grave soutenu, pas pour l'attaque. Pistes : filtre anti-pop et micro hors axe (acoustique, le plus
efficace) ; limiteur/compresseur **avec anticipation** (logiciel, à refaire) ; ALC matériel de
l'ES8311 (REG 0x18/0x19, vitesse d'attaque inconnue).

### Outils de debug TEMPORAIRES (à retirer avant de dériver le template)
- `hal/audio.c/.h` : `audio_debug_rx_peaks()`, `audio_debug_rx_words()`, `audio_debug_timing()`
  (crêtes L/R brutes, 8 mots I2S bruts, durée max du callback, intervalle max entre IRQ,
  drapeaux sticky `TXSTALL`/`RXSTALL` du PIO) + chronométrage dans `dma_irq_handler`.
- `instruments/instrument.cpp` : lignes console `[MIC]` / `[RAW]` / `[OUT]` toutes les 200 ms
  (`min/max/mean` ADC, `int16-sat`).
- `hal/es8311.c` : dump des registres (`[ES8311] dump:`) après `es8311_power_up()`.
- Lecture des mots bruts : `[L:16][R:16]` ; L et R portent le même échantillon (`ADCDAT_SEL=0`),
  avec un décalage d'un échantillon entre slots (sans conséquence en mono).
- Valeurs de référence au repos : plancher ADC ≈ ±2 LSB (`ADC_SCALE` 4), ±16–250 selon gain/micro.
  Un `OR=0x00000000` permanent = ADC non cadencé ; une crête collée à 32768 = ADC saturé.

### Contrôles au clavier (console série USB, lus dans `Instrument::on_tick`)
Pas de capteurs branchés sur la carte de test : le mixage se règle au clavier.
`q`/`a` = niveau du sweep ±1.5 dB, `w`/`s` = niveau de la voix ±1.5 dB, `e`/`d` = gain TPA ±1 code
(le firmware affiche les valeurs). Le mixage passe par `on_encoder(1|2, ±1)` (encodeurs 1 et 2 si
branchés). Réglage « les deux s'entendent » trouvé : sweep −36.5 dB, voix +10.5 dB (soit un écart de
47 dB entre le sweep audible et le niveau de la voix : confirme le signal micro faible).

### État actuel de `Instrument` (≠ démo par défaut décrite plus haut)
`SWEEP_ENABLED = false` (micro seul), `MASTER_GAIN = 1.0`, voix 0 dB, filtre passe-haut actif,
`TPA_VOLUME = 0x14`. Aucun limiteur, aucun écrêtage volontaire (seule la conversion en `int16`
sature, par sécurité). Remettre avant de figer le template : `SWEEP_ENABLED = true`, gains 0.5/0.5,
`MASTER_GAIN = 0.5`, retirer le debug ci-dessus.

### Reste à faire
- Limiteur/compresseur à anticipation pour les plosives (cf. ci-dessus).
- Vérifier AVDD de l'ES8311 et le câblage OUTP/OUTN → TPA6130A2.
- Confirmer `0x10`/`0x11`/`0x1B` (valeurs Espressif non documentées dans le guide).
- Le bring-up affiche `Touch (0x37): MISSING / init failed` alors que le scan I2C le trouve (non lié).
- Retirer les outils de debug, remettre l'instrument par défaut, mettre à jour ce fichier.
