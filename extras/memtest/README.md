# memtest — validation flash QSPI + PSRAM (mis de côté, NON compilé)

`memtest.h/.c` : test de la flash (JEDEC, SFDP, erase/program/verify des derniers
secteurs, débits) et de la PSRAM APS6404L (détection CS1 sur GPIO 0/8, init QMI
quad, taille, bus, passes plein-volume, watchdog + n° d'étape en cas de blocage).
Jamais compilé par Claude ; flash validée sur le PCB (16 Mo), PSRAM non validée.

## Pourquoi c'est de côté
La PSRAM montée sur le PCB (APS6404L-**SQN**-SN) est la version **1,8 V** alimentée
en 3,3 V : hors spécification (abs. max 2,45 V), crash USB à la 1re lecture.
À remplacer par **APS6404L-3SQR-SN** (3,3 V, même boîtier/brochage) avant de retester.

## Pour le réactiver
1. Copier `memtest.h/.c` dans `hal/`.
2. `CMakeLists.txt` : ajouter `hal/memtest.c` aux sources et `hardware_flash` aux
   bibliothèques.
3. `main.cpp` : `#include "hal/memtest.h"`, puis, juste après la bannière et avant
   l'étape I2C, `memtest_report_t mem = {}; mem.psram_cs_pin = -1; memtest_run(&mem);`
   (et, si voulu, les lignes Flash/PSRAM dans le `BRING-UP SUMMARY`).
4. Reconfigurer CMake. Réglages : `MEMTEST_FLASH_WRITE=0` pour éviter l'usure flash.
