# meehaw live performer

Live-performance firmware for the M5Stack Cardputer ADV: two sets of eight tracks (chord, lead, pad,
bass, fx, percussion, vocal, beat), patterns generated from a MIDI library in the global key, one-shot
drums, streamed vocals and loops, synth tracks with a 303 character, a perform page with a looper,
per-track FX, gyro FX, a session recorder that renders to WAV, saved sets as device-native presets with a
preset manager, and a USB drive mode for the card. Design: [docs/SPEC.md](docs/SPEC.md) (§9/§10 are the
current rules and measurements). **Performer's manual: [docs/MANUAL.md](docs/MANUAL.md)**; every key:
[docs/KEYMAP.md](docs/KEYMAP.md); printable key stickers: [docs/stickers/](docs/stickers/).

## Share / for friends

New to it? **[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)**: what to buy, how to flash a release, how to fill
the card from the free bundle, what to press first, how to render your own vocals and import your own packs. The
flashable images and the free card bundle are on the
**[releases page](https://github.com/mihatm8/meehaw-live-performer/releases)**. Passing the project on yourself —
the code under MIT, the ROMs, which card content may leave the card and which may not — is
[docs/SHARING.md](docs/SHARING.md). Licence: [LICENSE](LICENSE) (MIT, Miha Ambroz) for the code, tools and docs;
the licences of the card content are inside each bundle (`lp/LICENSES.md`) and the third-party credits in
[docs/CREDITS.md](docs/CREDITS.md).

## Layout

```
firmware/liveplayer/   Arduino sketch (src/core dsp engine gen io ui rec platform)
firmware/carddrive/    second firmware for the spare OTA slot: the card as a USB drive (tools/flash_drive.sh)
firmware/cardloader/   serial file-transfer loader for filling the microSD over USB
firmware/desktop/      SDL2 desktop build of the same app (make; --selftest N)
tests/                 host tests: sh tests/run_all.sh
tools/                 import_library.py, prepare_content.py, write_indexes.py, shard_folders.py, card_sync.py,
                       card_pull.py, card_qa.py, make_bundle.sh, make_loops.py, make_clips.py, make_rap.py,
                       gen_presets.py (the synth preset bank), make_stickers.py (key stickers),
                       flash.sh, flash_drive.sh, serial_cmd.py, serial_capture.py (python venv in tools/.venv)
sdcard/lp/             card content built from your library (copy to the card root as /lp)
docs/                  MANUAL.md, KEYMAP.md, CARD_IMPORT.md, CREDITS.md, SPEC.md, LOOPS.md, RAP_BANK.md,
                       CLIP_GENERATOR.md, RESEARCH_*.md, LAYOUT_PROPOSAL.md (historical)
```

## Build and flash

```bash
sh tests/run_all.sh                      # host tests (clang++)
tools/flash.sh                           # compile, upload, 20 s of serial log
tools/flash_drive.sh                     # once: the USB drive firmware into the spare OTA slot (ota_1)
```
Board: `esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`
(esp32 core 3.3.12, M5Cardputer 1.1.1, M5Unified 0.2.26, M5GFX 0.2.33). The drive firmware adds
`USBMode=default` (TinyUSB).

## Card content

[docs/CARD_IMPORT.md](docs/CARD_IMPORT.md) has the three ways to fill the card (card reader, the serial loader,
the player's USB drive mode). Building the content from your own library:

```bash
tools/.venv/bin/python -I tools/import_library.py --midi ~/Documents/cardputer/Midi \
    --pack ~/Documents/cardputer/House --genre House --out sdcard      # ~5 min
python3 -I tools/shard_folders.py sdcard/lp --max 64                   # FAT-friendly subfolders
python3 -I tools/write_indexes.py sdcard                               # _index.txt per folder
sh tools/make_bundle.sh                                                # dist/meehaw-card-<date>.zip
```
Copy `sdcard/lp` to the card root, or fill the card over USB without removing it:
```bash
arduino-cli compile --fqbn <board> --build-path /tmp/loader firmware/cardloader && \
arduino-cli upload -p /dev/cu.usbmodem101 --fqbn <board> --input-dir /tmp/loader firmware/cardloader
tools/.venv/bin/python -I tools/card_sync.py /dev/cu.usbmodem101 sdcard/lp --exclude packs/Testpack --prune --verify 8
tools/flash.sh                           # back to the player
```
Or hold SHIFT + gen pattern for a second on the device: the card mounts on the computer as a USB drive
(ENTER on the device returns to the player). On a computer that blocks removable drives,
`tools/card_pull.py PORT /lp/packs/House/presets ./pulled` pulls files over the serial port instead.

## Bring-up over serial

The player answers one-line commands on the USB serial port (`tools/serial_cmd.py PORT "stat"`):
`k X Y` press a matrix key, `kd/ku X Y` hold/release, `note P V`, `tog T`, `gen S`, `loop T`, `play`, `demo`
(synthetic full load), `vol N`, `stat` (status, per-track state, render profile, stacks), `ls DIR`, `cat FILE`,
`get FILE` (binary pull), `drive` (reboot into the USB drive firmware), `rec list|start|stop|render N|cancel|delete N`
(the session recorder). The full table is at the end of [docs/KEYMAP.md](docs/KEYMAP.md).

## Keys

[docs/KEYMAP.md](docs/KEYMAP.md): row 1/2 = set 1/2 track triggers (tap = start/stop at the bar and select),
Beat layers, snare rush, kick cut; row 3/4 = parameter ± for the selected track (vol, reverb, echo, dist,
filter, length, tempo, key: press = step, hold = ramp), gyro, genre, save/load, generate, editor, perform;
physical space = SHIFT (select, ramps, synth page, acid, loop mode, preset manager, recorder, PLAY/STOP ALL,
set 2 generate, output volume on the kick cut keys, USB drive on gen pattern held). Battery readout bottom right.

## Credits

[docs/CREDITS.md](docs/CREDITS.md) lists every project whose code we used or that inspired this one, with its
licence and link: MiniAcid (MIT; the 303 character and the reverb topology), the M5Stack libraries (MIT), the
arduino-esp32 core (LGPL-2.1) over ESP-IDF (Apache-2.0) with FatFs and TinyUSB (MIT), ldrolez/free-midi-chords
(MIT), the author's own REAPER Live Player (the inspiration for the set / track model; nothing is exchanged with it,
the presets are device-native files, SPEC §9.29), the Cardputer projects we learnt from (GLIDE, cardputer-studio,
MicroGroove), the computer-side tools (Rubber Band, FFmpeg, Apple's voices) and the datasets we looked at and
left out. The licences of the card content are in `sdcard/lp/LICENSES.md`.
