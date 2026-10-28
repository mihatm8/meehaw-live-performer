# Credits and licences

What this firmware uses, what inspired it, and what we looked at and left out. Written 2026-10-16 (SPEC §9.22 / §9.23
wave; the music-theory references of the §9.25 theory generator added the same evening). For every entry the licence was read from the LICENSE file on disk where one exists (path given) and otherwise
from the project's public page (fetched 2026-10-16; the quoted identifier is what that page states). The licences of
the card content are a separate document: `sdcard/lp/LICENSES.md` (rules in docs/RESEARCH_LIBRARY_EXPANSION.md §6).

## Code we used or adapted

| Project | Licence | What we took | Link |
|---|---|---|---|
| **MiniAcid** — Hector Urtubia. Two TB-303-style synths and a TR-808-style drum machine for the Cardputer | **MIT** — `/Users/miha/Documents/cardputer/miniacid/LICENSE`: "Copyright (c) 2025 Hector Urtubia. Permission is hereby granted, free of charge, …"; the repository page shows "MIT license" | The reverb bus topology (`DrumReverb` comb layout, SPEC §5: combs scaled ×2 so pads do not sound like a drum room); the 303 *character* re-created inside our own `SynthVoice` — saturation inside the filter loop (SAT) and the accent rule (ACC), SPEC §9.22, research §5 option B; the drum editor ideas. No MiniAcid source file is compiled into the firmware; the copyright notice is kept here | https://github.com/urtubia/miniacid |
| **M5Unified** — M5Stack. Speaker, IMU, power and board support | **MIT** — `~/Documents/Arduino/libraries/M5Unified/LICENSE`: "MIT License, Copyright (c) 2021 M5Stack" (v0.2.26) | Linked: `Speaker.playRaw`, `M5.Imu`, `M5.Power` | https://github.com/m5stack/M5Unified |
| **M5GFX** — M5Stack. Display driver | **MIT** — `~/Documents/Arduino/libraries/M5GFX/LICENSE`: "MIT License, Copyright (c) 2021 M5Stack" (v0.2.33) | Linked through `platform/m5_gfx.h` | https://github.com/m5stack/M5GFX |
| **M5Cardputer** — M5Stack. Board library (keyboard matrix, pins) | The installed library v1.1.1 ships **no LICENSE file** and the repository page lists none of its own; its README's "License" section names only its dependencies ("M5GFX : MIT", "M5Unified : MIT"). Treated as M5Stack's MIT family — **unverified** for this repository itself | Linked: `M5Cardputer.Keyboard`, `M5Cardputer.begin` | https://github.com/m5stack/M5Cardputer |
| **arduino-esp32** — Espressif. Arduino core 3.3.12 for the ESP32-S3 | **LGPL-2.1** — the repository page lists "LGPL-2.1 license" (the installed core 3.3.12 carries no root LICENSE file; `libraries/BLE/LICENSE` only) | The whole runtime: FreeRTOS tasks, SD / FatFs VFS, OTA (`esp_ota_set_boot_partition`), USB CDC, `USBMSC` | https://github.com/espressif/arduino-esp32 |
| **ESP-IDF** — Espressif. The prebuilt framework beneath the core | **Apache-2.0** — the repository page lists "Apache-2.0 license" | Everything the core links (heap, VFS, OTA, TinyUSB glue) | https://github.com/espressif/esp-idf |
| **FatFs** — ChaN. The FAT driver inside ESP-IDF | **FatFs licence (1-clause BSD-style)** — `~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.12/include/fatfs/src/ff.h` (R0.15): "Copyright (C) 2022, ChaN, all right reserved. … Redistribution and use of FatFs in source and binary forms, with or without modification, are permitted provided that the following condition is met: 1. Redistributions of source code must retain the above copyright notice, this condition and the following disclaimer." | The card file system (POSIX descriptors on the VFS, `platform/sd_fs.*`); the drive firmware's `disk_initialize` | http://elm-chan.org/fsw/ff/ |
| **TinyUSB** — Ha Thach. USB device stack | **MIT** — `…/esp32s3-libs/3.3.12/include/arduino_tinyusb/tinyusb/src/tusb.h`: "SPDX-License-Identifier: MIT, Copyright (c) 2019 Ha Thach (tinyusb.org)"; the repository page shows "MIT license" | The USB mass-storage device of `firmware/carddrive` (SPEC §9.22) | https://github.com/hathach/tinyusb |
| **ldrolez/free-midi-chords** — Ludovic Drolez. Chord progression MIDI files | **MIT** — `LICENSE` inside `/Users/miha/Documents/cardputer/_downloads/free-midi-chords/free-midi-chords-20260314.zip`: "MIT License, Copyright (c) 2019 Ludovic Drolez"; its README: "All the MIDI files are licensed under the MIT license" | The `LDZ-` clips of the card (Chord, and derived Lead / Pad; `tools/import_ldrolez.py`) and the early test library (`tools/prepare_content.py`) | https://github.com/ldrolez/free-midi-chords (release v0.20260314) |
| **Live Player for REAPER** — the author's own ReaScript / ReaLearn / Playtime project; this firmware is inspired by it | The author's own work; no licence file in the checkout (`/Users/miha/Downloads/live_player-master`) | The track order, the variant rules (`pattern_variants.lua`), the `.lpset` file name and its key=value text form, the key filter, the gain staging targets. Nothing is exchanged with it: the firmware's presets are device-native files (SPEC §9.29, 2026-10-27), not an export for REAPER | private repository |

## Inspiration only (no code taken)

| Project | Licence | What we learnt | Link |
|---|---|---|---|
| **GLIDE Synth for the Cardputer ADV** — CHARL3X | **PolyForm Noncommercial License 1.0.0** — the README: "GLIDE is free to download and play under the PolyForm Noncommercial License 1.0.0"; distributed as a binary (`GLIDE.bin` in `dist/` and the releases), no source published | The idea of a pocket performance synth with fretless gliding, tilt as a controller, and a loop pedal that keeps time; shaped our perform page and the gyro FX. Nothing of it is in this repository | https://github.com/CHARL3X/GLIDE-Synth-Cardputer-ADV |
| **cardputer-studio** — YONGHU-YUAN. Groovebox firmware for the Cardputer (synth, sequencer, sampler, mic recorder) | **MIT** — the README: "Licensed under the MIT License" | Platform reference (docs/LAYOUT_PROPOSAL.md): voice counts, short drum lanes, fades against clicks, one shared delay buffer | https://github.com/YONGHU-YUAN/cardputer-studio |
| **MicroGroove** — matoslav. Four-track groovebox for the Cardputer ADV with 303-style voices, 808 / 909 drums and a sampler | **MIT** — the README: "MIT — see LICENSE" | Platform reference: 22050 Hz mono, 256-sample buffers through `Speaker.playRaw`, a render task pinned to one core, a large internal-RAM sample pool | https://github.com/matoslav/MicroGroove |

## Tools used on the computer (not in the firmware)

| Tool | Licence | Use | Link |
|---|---|---|---|
| **Rubber Band Library** — Breakfast Quay | **GNU General Public License** (the page: "open source software under the GNU General Public License"; a commercial licence is sold separately) | The `rubberband` command-line tool stretches loops to 125 BPM and shifts vocals (`tools/make_loops.py`, `tools/shift_vocals.py`); nothing of it ships in the firmware or the card | https://breakfastquay.com/rubberband/ |
| **FFmpeg** | **LGPL 2.1 or later; GPL 2 or later when the optional GPL parts are enabled** (ffmpeg.org/legal: "GNU Lesser General Public License (LGPL) version 2.1 or later … some optional parts … are covered by the GNU General Public License (GPL) version 2 or later") | Decoding, resampling and trimming WAVs in the content tools; `atempo` as the fallback stretcher | https://ffmpeg.org/legal.html |
| **Apple macOS text-to-speech voices** (`say`) | Apple's macOS software licence: the voices are licensed for personal use on the Mac that renders them (docs/RAP_BANK.md "Voice licence note"); terms **unverified** beyond that | The `__RAP?__` vocal bank (`tools/make_rap.py`) — for the owner's own card only, never redistributed | bundled with macOS |
| **cairosvg** (optional) | LGPL-3.0 (PyPI metadata) | PDF export of the key stickers when `libcairo` is installed; `tools/make_stickers.py` falls back to its own PDF writer | https://cairosvg.org |

## Music-theory references (facts only; no text, data or files taken)

The on-device THEORY generator (SPEC §9.25) and the Mac clip generator share one hand-written table of minor-key chord
progressions, voicings and rhythm templates (`tools/theory_data.py`, emitted into the firmware by
`tools/make_theory_tables.py`). Chord progressions are musical facts, not copyrightable works; no page was downloaded,
scraped or quoted, and nothing of the references below ships in the firmware or on the card.

| Reference | Terms (as its page states them) | What we took | Link |
|---|---|---|---|
| **Hooktheory — the public theory pages / TheoryTab articles** on common chord progressions | Hooktheory's terms of use forbid bulk download and redistribution of its database (research §2.1: excluded as a *data* source, see "Consulted and NOT used"); reading the public articles is ordinary use and nothing was copied from them | The **names** of the common minor-key progressions of pop / house / EDM the table starts from — i–VI–III–VII, i–VII–VI–VII, i–iv–v, the Andalusian cadence i–VII–VI–V, the circle progression i–iv–VII–III–VI–ii°–V–i — and nothing else: the 126 entries, their weights and harmonic rhythm, the borrowed-chord rules (harmonic-minor V and vii°, the dorian IV, sus4 / sus2 colours), the voicing, bass and lead templates and the lead's Markov table are this project's own work | https://www.hooktheory.com/theorytab |
| **Common-practice harmony** (natural and harmonic minor, roman-numeral chord naming, inversions chosen for the least voice movement, drop-2 voicings, approach notes) | Public knowledge found in any harmony textbook; no particular text consulted or quoted | The rules the builders in `gen/variants.cpp` apply (`theoryPlan`, `theoryBuild`) | — |

## Test content (early bring-up only)

| Source | Licence | Status |
|---|---|---|
| **Dirt-Samples** (TidalCycles) 808 / 909 / vocal chops / fx | **No licence stated** — the repository has no LICENSE file and its README says nothing about licensing (checked on disk at `_downloads/dirt-samples/repo` and on the page) | Used only in the first test card built by `tools/prepare_content.py`; **private testing only**, never in a shared card image (docs/LAYOUT_PROPOSAL.md §6) | https://github.com/tidalcycles/Dirt-Samples |
| **Sonic Pi samples** (`etc/samples`) | **CC0 1.0** — `_downloads/sonic-pi-samples/repo/LICENSE.md` "Samples": "All the bundled samples are individually licensed under a CC0 1.0 Universal Public Domain Dedication" | Drum / percussion / fx one-shots of the first test card | https://github.com/sonic-pi-net/sonic-pi |
| **Groove MIDI Dataset v1.0.0** (Google Magenta) | **CC BY 4.0** — `_downloads/groove-midi/groove/LICENSE`: "This work is licensed under the Creative Commons Attribution 4.0 International License" | Two-bar drum slices of the first test card | https://magenta.tensorflow.org/datasets/groove |

## Consulted and NOT used

The library expansion study (docs/RESEARCH_LIBRARY_EXPANSION.md §2–§3, §6) looked at these sources; none was downloaded
or imported, and the reasons stand:

| Dataset | Licence (as its page states it) | Why not |
|---|---|---|
| **Lakh MIDI Dataset** | "distributed with a CC-BY 4.0 license" (the compilation) | The songs themselves are copyrighted pop MIDI transcriptions; GM style; key labels ≈ 60 % reliable; 1.77 GB |
| **POP909** | "MIT license" (the repository) | The licence covers the software / annotations, the 909 Chinese pop compositions stay copyrighted; expressive piano would need quantising |
| **PDMX** (Zenodo record 15571083) | "cc-by-4.0" | "Public domain" is uploader metadata; classical / choral material, little that fits a dance groovebox; 2.4 GB |
| **Chordonomicon** (Hugging Face) | "cc-by-nc-4.0" | Non-commercial: it would block any future sale of cards or firmware (research §6 rule 3); scraped from user sites |
| **Nottingham Music Database** (abc.sourceforge.net/NMD) | No licence or terms of use stated for the ABC files (only "the IPR associated with the original NMD format collection reside with Mick Peat" for the Ashover set) | Licence unverified; 75 % of the tunes are 6/8 folk material our 4/4 grid cannot use |
| **OpenEWLD** | Scores "intended to contain only Public Domain content"; the repository "MIT license"; Discogs-derived info "CC0", secondhandsongs-derived info "CC BY-NC 3.0" | Jazz-standard lead sheets, count unverified; left for a later phase |
| **Freesound** | Per sound: CC0, CC BY 4.0 or CC BY-NC 4.0 (FAQ) | Almost no key metadata (44 "vocal minor" hits), originals need OAuth2, many results are FX; a CC0-only harvest stays an option |

Also excluded on their terms of use (research §2.1): Hooktheory / TheoryTab (no bulk download or redistribution), BitMidi /
FreeMidi (no licence), Cymatics, Unison, Ghosthack and Function Loops (no redistribution of their sounds), Essen (status
"unclear"), Tegridy / LA-MIDI / MetaMIDI / GigaMIDI (non-commercial or research-only), Looperman (non-transferable).

## The commercial packs on the owner's card

The MIDI and audio packs the library was imported from (BGK, PTS, DHS, MRH, NJH, DPH, Synth-DM; GHUAC, 91UKG, PHV, SHV,
BPV, ASV, OSH, …) carry no licence file. Everything imported or derived from them — the library import, the `-TR`
transpositions, the `__LOOP__` loops, the `-SH` and `__CHP__` vocals — stays on the owner's card and Mac
(`sdcard/lp/LICENSES.md`, docs/CARD_IMPORT.md). The repository holds only this project's own code and tools.
