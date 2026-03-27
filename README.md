# Duke Nukem 3D ESP32 Port — ESP32-S3 + HUB75 LED Matrix Fork

This is a fork of [jkirsons/Duke3D](https://github.com/jkirsons/Duke3D) adapted to run on
**ESP32-S3 with only 2 MB PSRAM** driving a **64×64 HUB75 LED matrix** via
[bestander/duke3d-matrix-esphome](https://github.com/bestander/duke3d-matrix-esphome).

The original port targeted ODROID-GO / ESP32-WROVER (4 MB PSRAM, ILI9341 SPI LCD).
The changes here make it work within the tighter memory budget of the
Adafruit Matrix Portal S3 (ESP32-S3, 2 MB PSRAM, no SPI LCD).

---

## What changed from upstream

### Memory — 2 MB PSRAM budget

The original targets had 4 MB PSRAM. The Matrix Portal S3 has only 2 MB, shared between
the PSRAM BSS segment (static `EXT_RAM_ATTR` arrays) and the PSRAM heap (dynamic allocs).

| Change | File | Savings |
|--------|------|---------|
| `MAXSPRITES` 4096 → 2048 | `build.h` | −248 KB BSS |
| `MAXWALLS` 8192 → 4096 | `build.h` | −128 KB BSS |
| `MAXWALLSB` 2048 → 1024 | `engine.c` | −66 KB BSS |
| `tiles[MAXTILES]` made static `EXT_RAM_ATTR` array | `tiles.c` | avoids PSRAM heap alloc |
| `waloff[MAXTILES]` marked `EXT_RAM_ATTR` | `tiles.c/h` | moves 36 KB from DRAM to PSRAM BSS |

After these reductions the PSRAM heap pool grows from ~60 KB to ~540 KB, giving the
tile cache enough room to function.

### ESP32-S3 allocation fixes (`MALLOC_CAP_8BIT`)

`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` returns NULL on ESP32-S3 without
`MALLOC_CAP_8BIT`. All PSRAM heap allocations now use
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`.

### SD card path

The default `game_dir` was changed from `/sd/duke3d` to `/sdcard/duke3d` to match
the ESP-IDF 5.x FATFS mount point used by this project.

### EXT_RAM_ATTR annotations

Several large engine arrays have been annotated with `EXT_RAM_ATTR` so the linker
places them in the PSRAM BSS segment instead of internal DRAM (which is only ~200 KB
total on ESP32-S3 and is needed by WiFi, BT, and the HUB75 DMA driver).

---

## Target hardware

- **SoC**: ESP32-S3 (dual-core, 240 MHz, 8 MB flash, 2 MB Octal PSRAM)
- **Board**: Adafruit Matrix Portal S3
- **Display**: Two 64×32 HUB75 panels wired as a 64×64 logical display
- **Storage**: microSD card (SPI)
- **Framework**: ESP-IDF 5.2.1 via ESPHome 2024.11

The engine renders at 320×200 (8-bit indexed), downscaled 5:1 to 64×40 on the matrix,
with a 24-pixel HUD strip on the remaining rows.

---

## Original README

### Duke Nukem 3D ESP32 Port (jkirsons)
An ESP32 port of Duke Nukem 3D - based on the Win/Mac/Linux port:
Chocolate Duke3D (https://github.com/fabiensanglard/chocolate_duke3D)

#### Original requirements
- An ODROID-GO, or
- ESP32 WROVER (4 MB PSRAM) + ILI9341 SPI LCD + SD Card

#### Data files
The data files from Duke Nukem 3D (Atomic Edition v1.5 or lower) are required.
You can buy the game at https://www.zoom-platform.com/#store-duke-nukem-3d-atomic-edition

Place all game files into a folder called `duke3d` on the SD card.

---

## LEGAL STUFF

"Duke Nukem" is a registered trademark of Apogee Software, Ltd. (a.k.a. 3D Realms).
"Duke Nukem 3D" copyright 1996–2003 3D Realms. All trademarks and copyrights reserved.

### Build Engine
Folder: **`components/Engine`**

Licensed under the Build License — see `BUILDLIC.TXT`.

       // "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
       // Ken Silverman's official web site: "http://www.advsys.net/ken"

### Game Code
Folder: **`components/Game`**

Licensed under GNU General Public License v2.0 (see `LICENSE`).
Chocolate Duke modifications: "do whatever you want with my code"
(https://github.com/fabiensanglard/chocolate_duke3D/issues/48)

### SDL Library
Folder: **`components/SDL`**

Parts of SDL licensed under the ZLIB license (see `LICENSE_ZLIB`).

### ESP32 Wrapper
Folder: **`main`**

Licensed under GNU General Public License v2.0 (see `LICENSE`).
