# Flashing the meehaw live performer

How to put the firmware on an M5Stack **Cardputer ADV** (ESP32-S3, 8 MB flash) from a release folder or a GitHub
release: with esptool on macOS, Linux or Windows (§2), or from Chrome / Edge with the web flasher (§3). The firmware
alone shows the player; it plays nothing until the microSD card has the content in `/lp` — docs/GETTING_STARTED.md (first
steps) and docs/CARD_IMPORT.md (the three ways to fill the card). Building the release is `sh tools/make_release.sh`
(it compiles the three sketches and packs `dist/release-<ver>/`); publishing it is `tools/gh_release.py` (a draft
GitHub release with every file of that folder).

## 1. The files

| file | flash address | what it is |
|---|---|---|
| `meehaw-player-<ver>-full.bin` | **0x0** | **The one file a new device needs.** Bootloader @0x0, partition table @0x8000, otadata (boot_app0) @0xe000, the **player** @0x10000 (ota_0) and the **USB drive firmware** @0x340000 (ota_1), merged with `esptool merge-bin --chip esp32s3`. About 4 MB; flashing it leaves the rest of the 8 MB untouched. |
| `meehaw-player-<ver>-app.bin` | 0x10000 | the player alone — an **update** of a device that already has the full image |
| `meehaw-carddrive-<ver>-app.bin` | 0x340000 | the USB drive firmware alone (the card as a removable drive, MANUAL §21) |
| `meehaw-cardloader-<ver>-app.bin` | 0x10000 | the serial **card loader** for `tools/card_sync.py` (CARD_IMPORT.md B). It **replaces the player** in ota_0; flash the player app again afterwards |
| `SHA256SUMS` | — | `shasum -a 256 -c SHA256SUMS` (macOS) / `sha256sum -c SHA256SUMS` (Linux) checks every file |
| `index.html`, `manifest.json` | — | the web flasher page (§3) |
| `FLASHING.md`, `GETTING_STARTED.md`, `MANUAL.md`, `KEYMAP.md`, `sticker-keys-*.pdf` | — | this file, the first steps, the manual, every key, printable key stickers |
| `meehaw-card-free-<...>.zip` (when the release has one) | — | the shareable part of the card content; everything else on the author's card comes from commercial packs and is not shared (docs/RESEARCH_LIBRARY_EXPANSION.md §6) |

The partition table is the esp32 core's `default_8MB`: `nvs` 0x9000, `otadata` 0xe000, `app0` (ota_0) 0x10000, `app1`
(ota_1) 0x340000, 3 342 336 bytes (0x330000) per app slot, `spiffs` 0x670000 (unused), `coredump` 0x7F0000. The
device boots the slot `otadata` names: the full image's boot_app0 names ota_0 = the player; the player switches to ota_1
for the drive mode (SHIFT + gen pattern held 1 s) and the drive firmware points `otadata` back at ota_0 as its first act,
so any reset returns to the player.

## 2. esptool (macOS, Linux, Windows)

### 2.1 Install esptool

Python 3.10 or newer, then one of:

```sh
python3 -m pip install --upgrade esptool        # any system (Windows: py -m pip install --upgrade esptool)
pipx install esptool                            # keeps it in its own environment
brew install esptool                            # macOS with Homebrew
```

`esptool version` must answer. **v5** uses hyphenated commands (`write-flash`, `erase-flash`, `flash-id`), **v4**
underscores (`write_flash`, …) and the `esptool.py` name; everything below is v5 syntax — swap the hyphen for an underscore
on v4. The esp32 Arduino core 3.3.12 ships its own copy at
`~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool` (macOS), which works the same way.

### 2.2 Find the port

Connect the Cardputer with a USB-C **data** cable (a charge-only cable shows no port) straight into the computer and
switch it on. The ESP32-S3's built-in USB port enumerates as **USB JTAG/serial debug unit** without any driver:

| system | port | how to find it |
|---|---|---|
| macOS | `/dev/cu.usbmodem2101` (the number follows the USB socket) | `ls /dev/cu.usbmodem*` |
| Linux | `/dev/ttyACM0` | `ls /dev/ttyACM*`; your user must be in the `dialout` group (`sudo usermod -aG dialout $USER`, log in again). If the port vanishes at once, `brltty` or ModemManager grabbed it: `sudo apt remove brltty`, or stop ModemManager |
| Windows 10 / 11 | `COM5` (any number) | Device Manager → Ports (COM & LPT) → "USB JTAG/serial debug unit (COMx)"; no driver to install |

Only one program may hold the port: close the Arduino serial monitor, `screen`, PuTTY or the web flasher's tab before
running esptool. `esptool --chip esp32s3 --port PORT flash-id` is a harmless first test: it prints the chip (ESP32-S3)
and the flash size (8MB) and resets the device.

### 2.3 A new device, or a full re-flash: the full image at 0x0

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodem2101 --baud 921600 write-flash 0x0 meehaw-player-0.9.0-full.bin
```

(Linux: `--port /dev/ttyACM0`; Windows: `--port COM5`, and `py -m esptool` if `esptool` is not on the PATH.) The
long form that spells out what the short one keeps: `--before default-reset --after hard-reset write-flash --flash-mode
keep --flash-freq keep --flash-size keep 0x0 <file>` — the flash mode / frequency / size stay what the bootloader header
inside the image says (dio, 80 MHz, 8 MB). After `Hard resetting via RTS pin...` the device restarts and shows the splash
`meehaw / live performer`. The full image does not erase the `nvs` area; a device that boot-loops from an earlier
experiment gets a clean slate with `esptool --chip esp32s3 --port PORT erase-flash` first (the microSD card is never
touched by any of this).

### 2.4 Updates: one app image at its address

```sh
esptool --chip esp32s3 --port PORT --baud 921600 write-flash 0x10000  meehaw-player-0.9.0-app.bin       # the player
esptool --chip esp32s3 --port PORT --baud 921600 write-flash 0x340000 meehaw-carddrive-0.9.0-app.bin    # the drive firmware
esptool --chip esp32s3 --port PORT --baud 921600 write-flash 0x10000  meehaw-cardloader-0.9.0-app.bin   # the serial card loader
```

The player and the drive firmware keep each other: an update of one slot does not touch the other. The **card loader
replaces the player** (both live in ota_0): flash it, fill the card with `tools/card_sync.py PORT <folder>`
(CARD_IMPORT.md B), then flash `meehaw-player-<ver>-app.bin` at 0x10000 again. In the repository `tools/flash.sh`
(compile + upload the player) and `tools/flash_drive.sh` (the drive firmware into ota_1) do the same from source.

### 2.5 If esptool cannot connect: the BOOT button

`A fatal error occurred: Failed to connect to ESP32-S3` or no port at all: the chip is not answering on its USB port
(a crashed firmware, the drive firmware's own USB stack, a wrong port). Put it into the ROM download mode by hand:

1. Hold the side button **BtnA** — it is wired to **G0**, the ESP32-S3's BOOT strapping pin.
2. While holding it, press **reset** (or plug the USB cable in, or switch the device on). Release BtnA.
3. The device now sits in the download mode: the screen stays dark, the port shows up as `USB JTAG/serial debug unit`
   on every system. Run the `write-flash` command from §2.3 or §2.4.
4. Press reset (or power-cycle) afterwards if the device does not restart by itself.

That path works whatever firmware is in the flash. It is also the way out of a device that the **drive firmware** owns
(§5), and the way to flash a device whose player crashes at boot.

### 2.6 Verify: the serial boot line

The player talks on the same USB port at **115200** baud. Open a serial monitor and press reset:

```sh
screen /dev/cu.usbmodem2101 115200                       # macOS / Linux; quit with ctrl-a k
python3 -m serial.tools.miniterm /dev/cu.usbmodem2101 115200   # pyserial (esptool installs it); quit with ctrl-]
```

Windows: the Arduino IDE's serial monitor or PuTTY (Serial, COMx, 115200). Within about two seconds of the reset the
player prints its boot line:

```
[boot] heap free=13219 largest=8192 sd=1
```

`sd=1` means the card mounted; `sd=0` is a missing or unformatted card (FAT32, folder `lp` at the root). The numbers are
the free heap after boot (the player refuses to start below 8 KB free / 6 KB largest — `LOW RAM` on the screen). Then
type `stat` and Enter: one line of state (`[stat] vol=… bpm=125 … heap=… underruns=0`), which is the player answering. The
repository's `tools/.venv/bin/python -I tools/serial_cmd.py PORT stat` does the same, and `tools/flash.sh` captures 20 s
of this log after each upload. If nothing arrives, press reset with the monitor open (the boot line is printed once).

## 3. The web flasher (Chrome / Edge)

`index.html` next to the image is an [ESP Web Tools](https://esphome.github.io/esp-web-tools/) page: it reads
`manifest.json` (`chipFamily` ESP32-S3, the full image at offset 0) and flashes over **Web Serial**. It needs a desktop
Chrome or Edge (Firefox and Safari have no Web Serial) and a page served over **https or localhost** — opening the file
with `file://` does not work. The simplest way, from the release folder:

```sh
cd meehaw-release-0.9.0          # the folder with index.html, manifest.json and meehaw-player-0.9.0-full.bin
python3 -m http.server 8000      # then open http://localhost:8000 in Chrome
```

CONNECT → pick the `USB JTAG/serial debug unit` port in the browser's dialog → INSTALL. The "erase device" option of a
full install wipes the flash (not the card) first — take it for a device that comes from another firmware. Keep the tab
in front until it says done (about a minute). The port does not show up: §2.2 and §2.5 apply (hold BtnA while plugging
in to get the download mode; press ENTER on a device that is in drive mode, §5).

**Hosting it**: put `index.html`, `manifest.json` and the `.bin` in one folder of any https site. The browser fetches the
image through the manifest, so the image must come from the same origin as the page (or from a server that sends CORS
headers; the download links of GitHub release assets do not, so the manifest cannot point at them). **GitHub Pages**
works: copy the three files into a `gh-pages` branch (or the folder Pages serves, Settings → Pages), and the page is at
`https://mihatm8.github.io/meehaw-live-performer/`. Pages on a private repository needs a paid GitHub plan — the
repository has to be public for a free account; until then the `http.server` way above is the one to use.
`tools/make_release.sh` writes `manifest.json` for the built version and copies `tools/webflash/index.html` beside it.

## 4. The card after the firmware

The player boots to the main screen and says `NO SD CARD` when the card or its `lp` folder is missing. The content goes
to `/lp` on a FAT32 microSD card: GETTING_STARTED.md walks through the first fill, CARD_IMPORT.md has the three methods
(card reader, the serial loader, the player's USB drive mode). A release may ship `meehaw-card-free-<...>.zip` with the
shareable library (this project's GEN clips and the MIT chord progressions of ldrolez/free-midi-chords); the one-shots,
loops and vocals of the author's card come from commercial packs and stay there — the tools in the repository build the
same structure from your own packs.

## 5. Coming back from the USB drive mode

In drive mode the device is a USB disk (MANUAL §21): its serial port is the drive firmware's own TinyUSB port with a
**different name**, and esptool's automatic reset does not reach the bootloader through it. Return to the player first:

1. **Eject** the drive on the computer (Finder eject, `diskutil eject`, "safely remove"). The device shows `EJECTED`.
2. Press **ENTER** on the device (`ent`, matrix (13, 2)) — before an eject it asks for a second press within 3 s, since
   unflushed host caches would be lost. The device reboots into the player.
3. Alternatives: the command `player` on the drive's serial port (`tools/serial_cmd.py PORT player`), the reset button,
   or a power cycle — the drive firmware re-points `otadata` at the player (ota_0) as its first act, so every reset lands
   in the player. If a device ever boots into the drive firmware on its own, one more reset is the fix.

Then flash as in §2. If the drive firmware is the only thing that answers (no ENTER, no player port), §2.5's BOOT button
reaches the ROM loader regardless.

## 6. Trouble

| symptom | cause / fix |
|---|---|
| `Failed to connect`, `No serial data received` | wrong port, another program holds it, or the chip is not answering: §2.5 |
| `Serial port busy` / `Permission denied` | close the monitor or the browser tab that has the port; Linux: `dialout` group |
| `Wrong chip` / `This chip is ESP32-S3 not ESP32` | pass `--chip esp32s3` (the Cardputer ADV is an S3) |
| a black screen after flashing | the device stayed in download mode: press reset or power-cycle. Still black: §2.3 with `erase-flash` first |
| the drive firmware comes up instead of the player | press reset / ENTER (§5); if it repeats, flash the player app at 0x10000 again |
| `NO SD CARD` on the screen | the firmware is fine; the card (§4) is not |
| `NO DRIVE FIRMWARE` when holding SHIFT + gen pattern | the device only has the player app: flash `meehaw-carddrive-<ver>-app.bin` at 0x340000 (or the full image) |
| the web flasher says Web Serial is not supported / not allowed | a browser without Web Serial, or the page is not on https / localhost: §3 |
| the web flasher stalls at "Preparing installation" | a second tab or program has the port; the device is in drive mode (§5); try a different cable or USB socket |

Miha Ambroz (GitHub mihatm8), repository https://github.com/mihatm8/meehaw-live-performer.
