# Sharing meehaw with friends

Three things leave this Mac: the **code** (the GitHub repository), the **ROMs** (a GitHub release) and the **card
content** (the free bundle on that release). A friend's side of it is [GETTING_STARTED.md](GETTING_STARTED.md); this
page is yours: what to share, what must stay on your own card, and the commands.

| What | Where it goes | Who may have it |
|---|---|---|
| Code, tools, docs, tests, key stickers | `github.com/mihatm8/meehaw-live-performer`, MIT (`LICENSE`) | anyone, once the repository is public |
| ROMs: full flash image, player image, USB drive image | a GitHub release of that repository, or the release folder sent directly | anyone |
| Free card bundle `meehaw-card-free-<date>.zip` | the same release | anyone |
| Full card `dist/meehaw-card-20261016.zip` | nowhere: your own cards | you |
| Pack-derived clips, loops, vocals and the Apple-voice RAP bank | nowhere (they are inside the full card only) | you |

## 1. The code: the repository

Repository: https://github.com/mihatm8/meehaw-live-performer (private today, personal account `mihatm8`).

**Make it public:** open the repository on GitHub → **Settings** → **General** → scroll to **Danger Zone** →
**Change repository visibility** → **Change to public** → type the repository name to confirm. Everything in it,
including the history and every release, becomes readable by anyone.

**Or keep it private and invite people:** **Settings** → **Collaborators** → **Add people** (a GitHub account each;
they get push access too, so for read-only friends the public repository is the simpler choice). A private
repository's releases are visible only to collaborators.

**What a clone holds:** `firmware/liveplayer` (the player), `firmware/carddrive` (the USB drive firmware),
`firmware/cardloader` (the serial loader), `firmware/desktop` (the SDL build), `tools/`, `tests/`, `docs/` with the
manual, the key map and the printable stickers. **What it does not hold** (`.gitignore`): `sdcard/lp/` and
`sdcard/lp_stage/` — the card image and its staging mirrors, i.e. every clip, one-shot, loop and vocal; `dist/` — the
bundles; `tools/.rap_cache/` — the rendered Apple-voice words; `tools/.venv/`; `*.bin`. So the repository carries no
pack content, no synthesised voice audio and no binaries; a friend builds from source (README "Build and flash") or
takes the ROMs from the release.

Before flipping the switch: `git status` must show no `sdcard/` or `dist/` file, and `git log --stat | grep -ci wav`
should print 0 (nothing of the card was ever committed). `sdcard/lp_free/` — the free stage that
`tools/make_synth_kit.py`, `tools/make_clips.py` and `tools/make_bundle.sh --free` write (§3) — is not in `.gitignore`;
add it unless you want ten thousand MIDI files in the history (the bundle on the release is how that content ships; its
hand-written `lp/LICENSES.md` is the one file in it worth keeping a copy of). `docs/CREDITS.md` quotes paths on this
Mac (`/Users/miha/...`); harmless, scrub them if you prefer.

**The licence.** `LICENSE` is MIT, "Copyright (c) 2026 Miha Ambroz". It covers what is this project's own: the
firmware, the tools, the docs, the tests, the generated clips (`GEN`), the preset bank and the theory tables. It does
not relicense the parts that keep their own terms, which `docs/CREDITS.md` lists: MiniAcid (MIT, its notice stays in
CREDITS), the M5Stack libraries (MIT), arduino-esp32 (LGPL-2.1), ESP-IDF (Apache-2.0), FatFs, TinyUSB (MIT), the
ldrolez chord files (MIT, their notice is in `lp/LICENSES.md` inside every bundle) — and it never covers the card-only
material of §3. MIT means anyone may use, change, redistribute and sell it as long as the copyright line travels
along. To change the licence later: replace the text of `LICENSE` (GPL-3.0 if changed versions must stay open,
PolyForm Noncommercial 1.0.0 if commercial use should be forbidden — GLIDE's choice), update the README line and this
page, commit. The new licence applies from that commit on; copies people already received under MIT stay MIT, and
the third-party notices stay whatever you choose.

## 2. The ROMs: a GitHub release

```sh
sh tools/make_release.sh                 # builds the player, the drive firmware and the merged full image into the release folder,
                                         # copies the docs, LICENSE, CREDITS.md and the newest dist/meehaw-card-free*.zip (§3) beside them
python3 -I tools/gh_release.py           # creates a DRAFT release on GitHub from that folder (gh, logged in as mihatm8)
```

Build the free bundle first (§3, three commands) so the release picks it up: `make_release.sh` copies card content, it
never builds any.

Flash layout the images follow (`default_8MB`, SPEC / `tools/flash_drive.sh`): bootloader at `0x0`, partition table
at `0x8000`, `boot_app0` at `0xe000`, the player (`ota_0`) at `0x10000`, the drive firmware (`ota_1`) at `0x340000`.
The **full image** is those five merged into one file (esptool `merge-bin`) that a friend writes at `0x0` in one go;
the **player image** alone, written at `0x10000`, is the update path; the **drive image** alone goes to `0x340000`.
Board for all three: `esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`
(the drive adds `USBMode=default`), esp32 core 3.3.12.

**What the draft contains:** the four images with a `SHA256SUMS` beside them, the free card bundle
`meehaw-card-free-<date>.zip` and its manifest, the docs (`FLASHING.md`, `GETTING_STARTED.md`, `CARD_IMPORT.md`,
`MANUAL.md`, `KEYMAP.md`, the sticker PDFs, the web flasher), `LICENSE` and `CREDITS.md` as files of their own, and
release notes that name the version, the board/core, the offsets and point to those files (the firmware binaries link
MIT, LGPL-2.1, Apache-2.0, FatFs and TinyUSB components whose notices must accompany binary copies, so the licence
and the credits travel with them, GitHub or not).

**Publishing it:** a draft is visible only to you. GitHub → the repository → **Releases** → the draft → check the
assets and the notes → **Publish release**. The link friends get is
https://github.com/mihatm8/meehaw-live-performer/releases (on a private repository only collaborators can open it).

**Without GitHub:** send the release folder itself (zip it; AirDrop, a drive link). It holds the same images, the free
bundle, the docs, `LICENSE` and `CREDITS.md`, so a friend needs nothing else — the `SHA256SUMS` file lets them check
the copy.

Never put the full card `dist/meehaw-card-<date>.zip` on a release or in a shared folder (§3); `meehaw-card-free-<date>.zip`
is the one that goes.

## 3. The card content: what may leave and what may not

Every derived file carries a token in its name (`sdcard/lp/LICENSES.md`); the rules are
docs/RESEARCH_LIBRARY_EXPANSION.md §6.

| Token / folder | Source | Status |
|---|---|---|
| `__GEN-…` MIDI (Chord, Lead, Pad; the release adds Beat, Percussion, Bass, Fx) | `tools/make_clips.py`, this project's own, seeded and regenerable | **free** |
| `__LDZ-…` MIDI (Chord, derived Lead / Pad) | ldrolez/free-midi-chords, MIT (notice in `lp/LICENSES.md`) | **free** |
| Starter drum kit `packs/Starter/drum/<role>/` | synthesised by `tools/make_synth_kit.py`, no sample taken from anywhere | **free** |
| MIDI without a token, `-TR…` transpositions | your commercial MIDI packs (BGK, PTS, DHS, …) and their transposed copies | card only |
| `packs/House/drum`, `fx`, `inst` | your audio packs, imported by `tools/import_library.py` | card only |
| `packs/House/vocal`: pack names, `-SH…`, `__CHP__` | your vocal packs, shifted and chopped | card only |
| `packs/House/loops/**` (`__LOOP__`) | your loop library stretched to 125 BPM | card only |
| `__RAPM__` / `__RAPF__` / `__RAPR__` vocals | Apple `say` voices rendered on this Mac | card only |

**The free bundle** (`dist/meehaw-card-free-<date>.zip`, built from the stage `sdcard/lp_free/lp`; it unpacks to a single
`lp/` folder like the full bundle): `config.txt`, `LICENSES.md`, the generated clips for all seven MIDI categories (Beat
and Percussion under `NA`), the MIT chord pack, the synthesised Starter drum kit, every folder sharded and indexed. It
is MIDI apart from the kit, so it is a few tens of MB and copies to a card in a minute. Three commands build it, in this
order, and each may be run again (the two generators are seeded and rebuild their folders, the copy skips what is
there):

```sh
python3 -I tools/make_synth_kit.py --out sdcard/lp_free                             # the Starter kit -> lp/packs/Starter/drum/<role>/
python3 -I tools/make_clips.py --out sdcard/lp_free --counts Chord=0,Lead=0,Pad=0   # GEN Bass, Fx, Beat and Percussion clips
sh tools/make_bundle.sh --free --from sdcard/lp                                     # the GEN / LDZ Chord, Lead, Pad clips + config.txt
   # from your card into the stage, then shard, index, card_qa; refuses any MIDI without a GEN / LDZ token and any vocal WAV,
   # requires lp/LICENSES.md; zips -> dist/meehaw-card-free-<YYYYMMDD>.zip + .manifest.txt, which make_release.sh picks up
```

`make_bundle.sh` without `--free` is the private full card (below): no gates, and a name (`meehaw-card-<date>`) the
release script does not look for. The stage's `lp/LICENSES.md` is written by hand, not by a tool: keep it in step with
this page.

GENERATE, the THEORY keys, the synth page, acid mode, the perform page, the recorder and the preset manager all work
on the free bundle; the Vocal track and loop mode stay empty until a friend renders their own material (below).

**Not in it, and why** (research §6 rule 2): the commercial MIDI and audio packs you bought carry no licence file,
so private use is all that can be assumed — and a transposed, pitch-shifted, chopped or time-stretched copy is still
the pack's material. The RAP bank is rendered with Apple's macOS voices, which Apple licenses for personal use on the
Mac that renders them (docs/RAP_BANK.md "Voice licence note"; exact terms unverified), so the WAVs are not a sample
pack to pass around. Measured on the current card: free = 3 961 `GEN` clips + 6 192 `LDZ` clips; everything else
(7 882 WAVs and the ~11 600 remaining MIDI files) is card-only.

**The private full card**: `dist/meehaw-card-20261016.zip` (797.4 MB, 30 324 files, sha256
`21c1ad44cddb6d33e79e7deab6ca451c3f8d5442466463b22e578d893af116d0`, built by `tools/make_bundle.sh`) is for your own
Cardputers: a second device, a spare card, a restore. Keep it in `dist/` (gitignored) and on your own disks; its
manifest and the `lp/LICENSES.md` inside say the same.

**How friends get vocals, loops and their own kits** — on their own Mac, with the tools from the repository
(`brew install ffmpeg`; `rubberband` optional for the loops), writing next to the free bundle's `lp`:

```sh
# --genre on these tools names the pack that holds the drum kit: Starter on the free bundle. make_rap.py and make_loops.py
# default to House, which would make a second genre: the firmware treats every lp/packs/* folder as a genre, `k` rotates
# through them, and a pack without a kit has silent Beat / Percussion tracks.
# rap vocals: Apple's voices are on every Mac, so each friend renders a bank locally instead of
# receiving yours — nobody redistributes synthesised voice audio. ≈ 20 min, ~40 MB at the defaults
python3 -I tools/make_rap.py --out sdcard --genre Starter    # writes sdcard/lp/packs/Starter/vocal/<Key>min/
# their own drum one-shots / vocal phrases (a folder with one-shots/drum/{kicks,snares,claps,hats,cymbals,percussion}/*.wav,
# optional one-shots/vocal/*.wav with a bpm token in the name); use a SEPARATE --out: the tool first deletes
# <out>/lp/midi and <out>/lp/packs/<Genre>
python3 -I tools/import_library.py --midi ~/empty --pack ~/MyPack --genre MyPack --out mycard
cp -R mycard/lp/packs/MyPack sdcard/lp/packs/
# loops at 125 BPM from a loop library whose names carry a bpm (and a key) token, into the kit's pack again
python3 -I tools/make_loops.py --src ~/Loops --out sdcard --genre Starter
# after any of these: shard, index, verify
python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet
python3 -I tools/card_qa.py sdcard
```

`make_rap.py` calls the voices of its `VOICES` table (Rocko, Reed, Evan Enhanced, Eddy, Karen, Moira, Tessa, Allison
Enhanced, Zarvox, Trinoids, Fred); `say -v '?'` lists what a Mac has, the Enhanced ones download in System Settings →
Accessibility → Spoken Content → System voice → Manage voices, or the table is edited to the voices at hand.

## 4. Before you send anything

- `git status` / `git log` show nothing from `sdcard/` or `dist/`; `sdcard/lp_free/` is ignored or intended.
- The release carries `meehaw-card-free-<date>.zip`, never the full `meehaw-card-<date>.zip`; its `lp/LICENSES.md`
  lists only the free sections (GEN, LDZ, the Starter kit).
- The release folder holds `LICENSE`, `CREDITS.md`, `CARD_IMPORT.md`, `FLASHING.md` and `GETTING_STARTED.md` (`SHA256SUMS`
  lists them), and the release notes name them.
- The full image was flashed to a device from the release folder once and boots to the splash.
