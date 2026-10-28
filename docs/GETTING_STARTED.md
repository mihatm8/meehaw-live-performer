# Getting started with meehaw

A pocket live set on the M5Stack Cardputer ADV: two sets of eight tracks (chord, lead, pad, bass, fx, percussion,
vocal, beat), patterns generated in one key from a MIDI library on the microSD card, synthesised melodic tracks with
a 303 character, one-shot drums, a perform page with a looper, a recorder that renders to WAV. This page gets a new
device from the box to the first beat. The manual is [MANUAL.md](MANUAL.md), every key is [KEYMAP.md](KEYMAP.md).

## 1. What you need

- **M5Stack Cardputer ADV** (the ADV model: ESP32-S3, 8 MB flash, the keyboard with the IMU — the firmware was built
  and tested on it only) and its USB-C cable (a data cable, not a charge-only one).
- A **microSD card, 8–32 GB, FAT32**. Cards up to 32 GB come formatted FAT32 (SDHC); keep that format — do not
  reformat as exFAT, APFS or HFS+. A card reader for the computer makes the first fill fastest.
- The current **release**: https://github.com/mihatm8/meehaw-live-performer/releases — the flash images, the free
  card bundle `meehaw-card-free-<date>.zip` and `SHA256SUMS`. (If the release reached you as a folder, the same files are
  in it.)
- A computer with Chrome or Edge (for the web flasher) or Python 3 (for `esptool`). Headphones: the Cardputer has a
  3.5 mm jack and a small speaker.

## 2. Flash the firmware

[FLASHING.md](FLASHING.md) has the details and the screenshots; the short form:

**Web flasher** (Chrome / Edge, no install): plug the Cardputer in, open a Web-Serial flasher such as
https://esp.huhn.me or https://adafruit.github.io/Adafruit_WebSerial_ESPTool/, **Connect**, choose the port the
browser offers, add the release's **full image** at offset `0x0`, **Program**. The full image holds the bootloader,
the partition table, the player and the USB drive firmware, so one file at `0x0` is the whole device.

**esptool** (any OS): `pip install esptool`, then

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 write-flash 0x0 <the full image>.bin
```

The port is `/dev/cu.usbmodem*` on a Mac (`ls /dev/cu.usbmodem*`), `COMn` on Windows, `/dev/ttyACM0` on Linux.
esptool 5 spells the command `write-flash`; esptool 4 is `esptool.py … write_flash`. If esptool cannot connect, hold
the **side button** (BtnA, which is GPIO 0) while you plug the cable in, then run the command again. When the write
ends the device resets and shows the splash `meehaw / live performer`, then the main screen with `NO SD CARD` — the
card comes next.

Check a download first: `shasum -a 256 <file>` (Mac / Linux) or `certutil -hashfile <file> SHA256` (Windows) must
print the line in `SHA256SUMS`.

## 3. Prepare the card

1. Unzip `meehaw-card-free-<date>.zip`. It unpacks to one folder, `lp`.
2. Put the card in the reader and copy the whole `lp` folder to the **root** of the card, so the card has
   `/lp/config.txt`, `/lp/midi`, `/lp/packs`. Keep the folder name, the `sNN` subfolders and every file name as they
   are: the player reads the `_index.txt` files inside, not the directories, and the key and category are in the
   names. On a Mac run `dot_clean /Volumes/<card>` afterwards (removes the `._*` files macOS adds), then **eject**
   the card in the Finder before pulling it.
3. Power the Cardputer off, insert the card, power on.

Terminal version on a Mac (card mounted as `/Volumes/CARD`):

```sh
unzip -q meehaw-card-free-<date>.zip -d /tmp/meehaw && \
  rsync -a --exclude '.DS_Store' --exclude '._*' /tmp/meehaw/lp/ /Volumes/CARD/lp/ && \
  dot_clean /Volumes/CARD && diskutil eject /Volumes/CARD
```

What the free bundle holds: generated MIDI clips for all seven categories in every minor key, the MIT chord pack
(ldrolez/free-midi-chords), a synthesised Starter drum kit, `config.txt` and `LICENSES.md`. It is small (MIDI mostly).
The Vocal track and the loop mode have nothing to play until you add your own material (§5). [CARD_IMPORT.md](CARD_IMPORT.md)
has the two other ways to fill a card (over USB serial, and the player's own USB drive mode).

## 4. First boot: what you see and what to press

- The splash stays 1.5 s, then the **main screen**: two rows of eight squares — the top row is set 1, the bottom row
  set 2, one square per track in its colour (CHD LED PAD BAS FX PRC VOC BEAT). Filled = playing, outline = loaded and
  stopped, dim = empty. The **footer** shows the key, the tempo and the genre (the pack's folder name), e.g.
  `A MIN 125  …  LEN 4`, and the battery at the right. `NO SD CARD` means the card did not mount: FAT32, `lp` at the root, re-seat it, power-cycle.
- Nothing plays after a boot, by design. **SHIFT is the space key** (bottom right), held.
- **ent** = GENERATE set 1: the status line counts `GEN S1 1/8 … 8/8` and the squares of the top row fill with
  outlines. Then **tap the keys of the top row** — `` ` `` chord, `1` lead, `2` pad, `3` bass, `4` fx, `5` percussion,
  `6` vocal (empty on the free bundle), `7` beat — to start each track at the next bar (a tap also selects it). `8` `9` `0` `-` are the Beat layers
  (kick, +hat, +snare, full), `=` the snare rush, `del` the kick cut.
- **Volume**: **SHIFT + del** louder, **SHIFT + \\** softer (16 steps of 255; the footer shows `OUT 59%`). If the device
  is silent, that is the first thing to press; `volume=` in `lp/config.txt` sets the level at boot.
- **The theory keys**: **SHIFT + ;** composes set 1 from one chord progression on the device (**SHIFT + .** does set 2)
  — the chord, lead, pad, bass and fx tracks are written to fit each other, in the current key, without the library.
  Press again for another progression. Try this right after the first GENERATE: it is the quickest way to hear the
  player hang together.
- The rest, one key each: `'` new pattern for the selected track, `/` new sound, `k` next genre (pack), rows 3 and 4
  are the parameter keys of the selected track (`fn`/`ctl` volume, `sft`/`opt` reverb, `a`/`alt` echo, `s`/`z`
  distortion, `d`/`x` filter, `f`/`c` length, `g`/`v` tempo, `h`/`b` key; press = step, hold = ramp), `l` saves a
  preset, `;` loads one, `n` the step editor, `m` the perform page, **BtnA** (the side button) is play / stop all.
  `SHIFT + ent` generates set 2; `SHIFT + 8` / `9` / `0` / `-` open the synth page (nine knobs) of the chord / lead /
  pad / bass track of set 1.

MANUAL.md §24 is the one-page key reference; §23 explains every status message.

## 5. Make it yours: vocals, your own packs

The tools are in the repository (clone it, or download the zip from GitHub); they are plain Python 3 scripts plus
`ffmpeg` (`brew install ffmpeg` on a Mac). Work in a folder `sdcard` that holds the bundle's `lp`, then copy `lp` back
to the card.

- **Rap vocals** (Mac only; uses the system voices, `say`): `python3 -I tools/make_rap.py --out sdcard --genre Starter`
  renders a bank of keyed one-shots, loops and 8-bar sessions into `sdcard/lp/packs/Starter/vocal/<Key>min/` (about 20
  minutes at the defaults). `--genre` must be the pack that holds the drum kit — `Starter` on the free bundle; the
  script's own default, `House`, would make a second genre. The player treats every `lp/packs/*` folder as a genre, `k`
  rotates through them, and a pack without a kit has silent Beat and Percussion tracks. The voices it calls — Rocko,
  Reed, Evan (Enhanced), Eddy, Karen, Moira, Tessa, Allison (Enhanced), Zarvox, Trinoids, Fred — must be installed:
  `say -v '?'` lists yours; the Enhanced ones download in System Settings → Accessibility → Spoken Content → System
  voice → Manage voices, or edit the `VOICES` table at the top of the script.
  Apple licenses the voices for use on your own Mac, which is why everyone renders their own bank rather than
  passing WAVs around.
- **Your own drum kit / vocal phrases**: make a folder with `one-shots/drum/kicks`, `snares`, `claps`, `hats`,
  `cymbals`, `percussion` (WAV one-shots, any format ffmpeg reads) and optionally `one-shots/vocal` (phrases with a
  `120bpm`-style token in the name), then
  `python3 -I tools/import_library.py --midi ~/empty --pack ~/MyPack --genre MyPack --out mycard` (`--midi` is a
  folder of MIDI clips named `<Key>__<Category>__<name>.mid` if you have one, else an empty folder). It trims, balances and converts
  everything to 22050 Hz mono 16-bit. Use a **separate `--out`**: the tool deletes `<out>/lp/midi` and
  `<out>/lp/packs/<Genre>` first. Then `cp -R mycard/lp/packs/MyPack sdcard/lp/packs/` — the `k` key rotates through
  the packs.
- **Loops** (125 BPM, for loop mode): `python3 -I tools/make_loops.py --src ~/Loops --out sdcard --genre Starter` (the
  kit's pack again) from a loop library whose names carry a bpm token ([LOOPS.md](LOOPS.md); `brew install rubberband`
  for the better stretcher).
- **After any of these**, always:
  `python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet`
  (folders of at most 64 files, a fresh `_index.txt` everywhere), and `python3 -I tools/card_qa.py sdcard` to check
  every file the way the firmware reads it. Copy `sdcard/lp` to the card as in §3 (merge: the card's own
  `lp/state`, `lp/packs/*/presets` and `sets` folders hold your sessions and saved sets).

## 6. The manual and the key stickers

- [MANUAL.md](MANUAL.md): the performer's manual (§24 quick reference, §23 troubleshooting).
- [KEYMAP.md](KEYMAP.md): every key with its matrix position, main and SHIFT function.
- [stickers/keys-main.pdf](stickers/keys-main.pdf) and [stickers/keys-shift.pdf](stickers/keys-shift.pdf): A4 sheets
  of one sticker per key at 1:1 (main legend large / SHIFT function large). Print at **100 %**, never "fit to page",
  and check the 50 mm bar on the sheet. The key pitch assumed is 5.8 mm; measure yours (centre of `` ` `` to centre of
  `del`, divided by 13) and regenerate with `python3 -I tools/make_stickers.py --pitch <mm>` if it differs.

## 7. Updating

A new release normally changes only the player: write the **player image** at `0x10000` and nothing else —

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 write-flash 0x10000 <the player image>.bin
```

(or the same file at `0x10000` in the web flasher). The card, your sessions, presets and recordings are untouched.
The release notes say when the USB drive firmware changed too (it goes to `0x340000`) or when the full image is
needed again. If the player is in USB drive mode when you flash, leave it first (§8).

## 8. USB drive mode, and how to get back

Hold **SHIFT + '** for one second: `USB DRIVE...`, the device reboots into the drive firmware and the card appears on
the computer as a removable drive — copy, merge or back up the `lp` folder there without taking the card out. To
return: **eject** the drive on the computer (the screen says `EJECTED`), then press **ENTER** on the device; it reboots
into the player. ENTER before the eject asks for a second press; any reset (or re-plugging) also brings the player
back, because the drive firmware re-points the boot slot at the player the moment it starts. On some managed
computers no drive ever appears (removable media disabled by policy): use the card reader instead.
