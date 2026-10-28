# Putting the card content on a microSD card

The player reads everything from one folder at the root of the card: `/lp` (SPEC §6). The shared bundle
(`tools/make_bundle.sh` → `dist/meehaw-card-<YYYYMMDD>.zip`, with `meehaw-card-<YYYYMMDD>.manifest.txt` beside it)
unpacks to exactly that folder:

```
lp/
  config.txt                 key=value settings
  LICENSES.md                what may leave the card and what may not
  midi/<Category>/<Key>/     MIDI clips (Chord Lead Pad Bass Fx Beat Percussion), sharded into s00..s11
  packs/House/drum/<role>/   one-shots (kick snare clap hh oh cymbal perc)
  packs/House/vocal/<Key>/   vocal phrases, 22050 Hz mono 16-bit, a bpm token in every name
  packs/House/loops/<cat>/<Key>/   125 BPM loops for the loop mode (docs/LOOPS.md)
  packs/House/{fx,inst}/     unused by the firmware for now
```

Every content folder carries an `_index.txt` (tools/write_indexes.py); the firmware never walks a directory, it
reads those indexes. Keep the folder name `lp`, keep the shard folders `sNN`, do not rename files (the key label,
the `__` fields and the bpm / bars tokens are what the picker reads). The image merged on 2026-10-16 holds 30,738
files, 1.08 GB of bytes: 21,819 MIDI clips (5.7 MB) and 7,882 WAVs (1.06 GB). On a FAT32 card with 32 KB clusters
every small MIDI file costs one cluster, so the same content occupies about 1.9 GB there (`diskutil info
/Volumes/<card>` shows the cluster size as "Allocation Block Size").

Check the download first: `shasum -a 256 meehaw-card-<YYYYMMDD>.zip` must print the sha256 in the manifest.

**Free bundle or full card.** Two bundles share this layout and install the same way. `meehaw-card-free-<YYYYMMDD>.zip`
(on the GitHub release; docs/GETTING_STARTED.md) is the one friends get: the generated clips (`GEN`) for every
category, the MIT chord pack (`LDZ`), a synthesised Starter drum kit, `config.txt` and `LICENSES.md` — MIDI apart from
the kit, a few tens of MB, so it fits any card and copies in a minute; its vocal and loop folders stay empty until
you render your own (docs/SHARING.md §3). `meehaw-card-<YYYYMMDD>.zip` is the owner's full card: the same plus
everything derived from the commercial packs (`-TR`, `-SH`, `__CHP__`, `__LOOP__`, the library import, the one-shots)
and the Apple-voice RAP bank — card-only material (docs/RESEARCH_LIBRARY_EXPANSION.md §6) that is never put on a
release. The sizes above describe the full card.

## A. Card reader (fastest, recommended for the first fill)

1. Take the card out of the Cardputer (power off first) and put it in a reader. Keep the card's own format: a
   32 GB SDHC card comes as FAT32, which is what the firmware's SD library expects; do not reformat it as APFS /
   HFS+. (exFAT needs an SD library built with exFAT support — stay on FAT32 unless you know yours has it.)
2. Unzip the bundle and copy the whole `lp` folder to the **root** of the card (so the card has `/lp/config.txt`,
   `/lp/midi`, `/lp/packs`). If a `lp` folder is already there, replace it, or merge: the Finder's "Merge" keeps
   files that exist only on the card. The folders the device owns — `lp/state/` (the saved session), `lp/packs/*/sets/`
   and `lp/packs/*/presets/` (saved sets and presets) — are **not** in the bundle, so a merge never overwrites them;
   a replace does delete them (copy them aside first if you care). A `presets/` folder holds the
   `<Genre>_<Key>_<NNN>.lpset` text files (`lp.*` keys naming their sources by card path), their `<name>_t<i>.mid`
   pattern files (edited, recorded and THEORY tracks) and, for presets saved before 2026-10-27, an `assets/` folder
   of clip copies that the firmware still reads but no longer writes or deletes (SPEC §9.29).
3. macOS writes `.DS_Store` and `._*` AppleDouble files onto FAT volumes. They are harmless (dotfiles are never
   indexed) but they waste clusters and directory entries; `dot_clean /Volumes/<card>` removes them before you eject.
4. Eject the volume in the Finder (do not just pull the card), put it back in the Cardputer, power on. The player
   indexes the packs lazily on the first GENERATE; the status line says `NO CARD` if the card did not mount.

Terminal version of steps 2–4 (card mounted as `/Volumes/CARD`):

```sh
unzip -q meehaw-card-20261016.zip -d /tmp/meehaw && \
  rsync -a --exclude '.DS_Store' --exclude '._*' /tmp/meehaw/lp/ /Volumes/CARD/lp/ && \
  dot_clean /Volumes/CARD && diskutil eject /Volumes/CARD
```

## B. Over USB serial: `firmware/cardloader` + `tools/card_sync.py` (incremental updates)

The card stays in the device. A small loader firmware answers a line protocol on the USB serial port (PING, LS,
PUT, CRC, RM, …, see the top of `firmware/cardloader/cardloader.ino`) and `tools/card_sync.py` pushes a local
folder through it, so no disk is mounted on the computer at all.

```sh
# 1. flash the loader (the player is overwritten; tools/flash.sh brings it back afterwards)
arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled \
    --build-path /tmp/loader firmware/cardloader && \
arduino-cli upload -p /dev/cu.usbmodem101 --fqbn esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled \
    --input-dir /tmp/loader firmware/cardloader

# 2. sync (the local folder becomes /lp on the card)
tools/.venv/bin/python -I tools/card_sync.py /dev/cu.usbmodem101 sdcard/lp --exclude packs/Testpack --prune --verify 8

# 3. back to the player
tools/flash.sh
```

- `SRC_DIR` is the local folder that becomes `/lp` on the card (`--dest` changes the target). Files whose size
  already matches on the card are skipped, so a re-run after a small change only sends what changed.
- `--exclude rel/path` (repeatable) leaves a local folder out, e.g. `--exclude packs/Testpack` (the host tests'
  fixtures) or `--exclude packs/House/vocal` to postpone the 1 GB of vocals.
- `--prune` deletes card files that do not exist locally (inside the synced tree, never the protected folders);
  without it old files stay.
- `--verify N` reads back the CRC32 of N random copied files.
- `presets/`, `sets/` and `state/` are **protected**: never copied from the local folder and never pruned — they are
  the card's own (saved sets and presets, the session the device restores at boot, SPEC §9.8 / §9.18 / §9.29). A host test
  run leaves its own `sdcard/lp/state/*.lpset` behind; that is why they are never pushed.
- Speed: about 10 files/s and ≈ 130 KB/s measured (5,757 files / 69 MB in 9 min, SPEC §10). The full image —
  30.7 k files, 1.08 GB — would take roughly 2.5–3 hours this way, so use A or C for the first fill and B for the
  incremental updates it was written for (the MIDI tree alone, 21.8 k files / 5.7 MB, is about 35 minutes).
- `tools/card_pull.py PORT /lp/packs/House/presets ./pulled` pulls a folder back (works with the player firmware too).

## C. The player's USB drive mode (the card as a removable drive)

The player can hand the card to the computer as a USB mass-storage device, without taking the card out. It needs
the drive firmware in the spare OTA slot once: `tools/flash_drive.sh [port]` writes only `firmware/carddrive` to
`ota_1`; the player stays in `ota_0` and keeps booting as before (its status says `NO DRIVE FIRMWARE` if the slot is
empty).

1. On the device hold **SHIFT + gen pattern** (SHIFT is the physical space key, gen pattern is `'` at matrix (12, 2),
   docs/KEYMAP.md) for **1 second**. The status shows `USB DRIVE...` and the device reboots into the drive firmware
   (a short SHIFT + `'` press stays the normal gen pattern). The serial command `drive` (`tools/serial_cmd.py PORT drive`)
   does the same.
2. After 1–2 s the card mounts on the computer as a removable drive (USB re-enumerates, so the serial port gets a
   new name while the drive is active). Copy the `lp` folder from the unzipped bundle to the root of that drive,
   merging or replacing as in A (the same note about `state/`, `sets/`, `presets/` applies; `dot_clean` the volume
   before ejecting). Writing goes through the Cardputer's SPI card interface, so it is slower than a card reader;
   plan for the first 1 GB fill to take a while, or do the first fill with A and use C for updates.
3. **Eject** the drive on the computer (Finder eject or `diskutil eject`). The device screen shows `EJECTED`.
4. Press **ENTER** on the device: it reboots into the player. (ENTER before the eject asks for a second press, since
   un-flushed host caches would be lost; the serial command `player` or a reset also return to the player — the drive
   firmware re-points the boot slot at the player as its first act, so any reset is safe.)

Some managed computers (corporate MDM profiles) block external or removable drives: the device then enumerates but
no volume appears, or the volume is read-only. Use A on a computer that allows removable media, or B, which is a
serial port rather than a disk.

Verified on the device (2026-10-16, SPEC §9.27): `tools/flash_drive.sh` wrote the drive firmware to `ota_1` and the
player came back; the serial `drive` printed `[drive] ota_1 holds an app (...): switching`, the device rebooted into
the drive firmware and enumerated as `M5STACK_CARDPUTER` (Espressif 0x303a:0x1001) with a TinyUSB mass-storage
interface that macOS matched with its own storage driver — and on a Mac under a management policy that disables
external disks no volume appeared in `diskutil list`: exactly the symptom above (use A on another computer, or
`tools/card_pull.py` over the player's serial port). Two more facts: the first `drive` right after a boot can pass
without effect while the session writer is still busy (the player waits up to 3 s for a save, load, listing or session
write and then simply goes on) — try again a few seconds later; and the serial `player` on the drive's own port
returns to the player.

## After any of the three

- The indexes in the bundle are current; if you add or delete files yourself, run
  `python3 -I tools/write_indexes.py <card root> --prune` (the folder that holds `lp/`) so every `_index.txt`
  matches again, and `python3 -I tools/shard_folders.py <card root>/lp --max 64` if a folder grew past 64 files.
  `python3 -I tools/card_qa.py <card root>` verifies every file the way the firmware reads it and prints the
  per-key inventory.
- The firmware reaches at most 12 shards per folder (768 files at 64 per shard); `write_indexes.py --check` reports a
  folder that went past that.
- Licence: `lp/LICENSES.md`. Files with the `-TR`, `-SH`, `__CHP__` and `__RAP?__` tokens, the library import and the
  loops are for the owner's own card (docs/RESEARCH_LIBRARY_EXPANSION.md §6); `LDZ-` files are MIT, `GEN-` files are
  this project's own.
