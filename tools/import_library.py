#!/usr/bin/env python3
"""Import the REAPER Live Player library (Midi root + one pack) into the SD card layout.

Usage:
  python3 -I import_library.py --midi <Midi root> --pack <pack folder> --genre House --out <sdcard dir>

SD layout written:
  lp/midi/<Category>/<Key>/<Key>__...mid         REAPER names (Chord Lead Pad Bass Fx Percussion Vocal Beat), copied unchanged
  lp/packs/<Genre>/drum/<role>/*.wav             roles: kick snare clap hh oh cymbal perc
  lp/packs/<Genre>/vocal/<Key>/*.wav             full-length phrases, streamed by the firmware
  lp/packs/<Genre>/fx/*.wav, inst/<cat>/*.wav    extra one-shots (optional sampler use)
  lp/packs/<Genre>/sets/                         saved sets (.lpset) go here

All WAVs: 22050 Hz, mono, 16-bit PCM, silence-trimmed, faded, drum roles balanced with the
REAPER drum_balance.lua targets (loudest-20 ms RMS / peak per role, +4 dB trim, never boosted above
0 dBFS peak -0.5 dB).
"""
import argparse, math, os, re, shutil, subprocess, sys, wave
from array import array

RATE = 22050
KEY_RE = re.compile(r"^([A-G](?:#|b)?)(min|maj)?__", re.IGNORECASE)
FLAT = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#"}

# REAPER category folder names are kept verbatim so the tree can be copied back into ~/Music/Midi
CATEGORY_DIRS = {"Chord": "Chord", "Lead": "Lead", "Pad": "Pad", "Bass": "Bass", "Fx": "Fx",
                 "Percussion": "Percussion", "Beat": "Beat", "Vocal": "Vocal"}

# role -> (max_ms, rms_target_db, peak_target_db)   (REAPER drum_balance.lua profiles)
ROLES = {
    "kick":   (400, -15, -9),
    "snare":  (350, -20, -12),
    "clap":   (300, -21, -13),
    "hh":     (150, -27, -18),
    "oh":     (400, -26, -17),
    "cymbal": (600, -26, -18),
    "perc":   (300, -23, -16),
}
TRIM_DB = 4.0

def key_folder(name):
    m = KEY_RE.match(name)
    if not m:
        return "NA" if name.upper().startswith("NA__") else None
    root = m.group(1)
    root = root[0].upper() + root[1:]
    root = FLAT.get(root, root)
    return root + ("min" if (m.group(2) or "").lower() == "min" else "")

def ffmpeg_pcm(src, max_seconds):
    cmd = ["ffmpeg", "-v", "error", "-i", src, "-t", str(max_seconds), "-ac", "1", "-ar", str(RATE),
           "-f", "s16le", "-acodec", "pcm_s16le", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    a = array("h"); a.frombytes(data[: len(data) - len(data) % 2])
    return a

def rms_db_loudest(a, window=int(RATE * 0.02)):
    best = 0.0
    if len(a) < window:
        window = max(1, len(a))
    acc = 0
    for i in range(window):
        acc += a[i] * a[i]
    best = acc
    for i in range(window, len(a)):
        acc += a[i] * a[i] - a[i - window] * a[i - window]
        if acc > best:
            best = acc
    rms = math.sqrt(best / window) / 32768.0
    return 20 * math.log10(max(rms, 1e-6))

def write_wav(dst, a):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with wave.open(dst, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(a.tobytes())

def process(src, dst, max_ms, rms_target=None, peak_target=None, fade_ms=12, trim_db=-54.0):
    a = ffmpeg_pcm(src, max_ms / 1000 + 0.3)
    if len(a) < RATE * 0.005:
        return None
    thr = int(32767 * 10 ** (trim_db / 20))
    start = 0
    while start < len(a) and abs(a[start]) < thr:
        start += 1
    start = max(0, start - int(RATE * 0.002))
    a = a[start: start + int(RATE * max_ms / 1000)]
    end = len(a)
    while end > 0 and abs(a[end - 1]) < thr // 2:
        end -= 1
    a = a[: max(end, int(RATE * 0.01))]
    peak = max(1, max(abs(x) for x in a)) / 32768.0
    if rms_target is not None:
        rms = rms_db_loudest(a)
        gain = min(1.0, 10 ** (peak_target / 20) / peak, 10 ** (rms_target / 20) / (10 ** (rms / 20)))
        gain *= 10 ** (TRIM_DB / 20)
        gain = min(gain, 10 ** (-0.5 / 20) / peak)       # never clip
    else:
        gain = 10 ** (-1.0 / 20) / peak                   # plain peak normalise
    n = len(a); fade = min(n // 2, int(RATE * fade_ms / 1000)); attack = int(RATE * 0.001)
    for i in range(n):
        g = gain
        if i >= n - fade:
            g *= (n - i) / fade
        if i < attack:
            g *= i / attack
        a[i] = max(-32768, min(32767, int(a[i] * g)))
    write_wav(dst, a)
    return len(a) * 2

def import_midi(midi_root, out):
    counts = {}
    for cat, folder in CATEGORY_DIRS.items():
        src_dir = os.path.join(midi_root, cat)
        if not os.path.isdir(src_dir):
            continue
        for dp, _, fs in os.walk(src_dir):
            target_cat = folder
            if os.path.basename(dp).lower() == "bass derived":
                target_cat = "Bass"
            for f in fs:
                if not f.lower().endswith((".mid", ".midi")):
                    continue
                key = key_folder(f)
                if key is None:
                    counts["skipped_unparseable"] = counts.get("skipped_unparseable", 0) + 1
                    continue
                if target_cat in ("Beat", "Percussion"):
                    key = "NA"                      # drums ignore the key (SPEC §9.9)
                dst = os.path.join(out, "lp", "midi", target_cat, key, f)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(os.path.join(dp, f), dst)
                counts[target_cat] = counts.get(target_cat, 0) + 1
    return counts

def hat_role(name):
    n = name.lower()
    return "oh" if re.search(r"open|ohh|_oh\b|ohat|oh\.wav$", n) else "hh"

def import_pack(pack, genre, out, limit=None):
    base = os.path.join(out, "lp", "packs", genre)
    os.makedirs(os.path.join(base, "sets"), exist_ok=True)
    report = {}
    def run(folder, role_of, dst_of, max_ms_of, balance):
        n = 0; total = 0; skipped = 0
        files = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
        if limit:
            files = files[:limit]
        for f in files:
            if not f.lower().endswith(".wav"):
                continue
            role = role_of(f)
            max_ms, rms_t, peak_t = ROLES[role] if role in ROLES else (max_ms_of, None, None)
            dst = dst_of(role, f)
            try:
                size = process(os.path.join(folder, f), dst, max_ms, rms_t if balance else None, peak_t if balance else None)
            except subprocess.CalledProcessError:
                size = None
            if size:
                n += 1; total += size
            else:
                skipped += 1
        return (n, total, skipped)

    one = os.path.join(pack, "one-shots")
    for sub, role in (("kicks", "kick"), ("snares", "snare"), ("claps", "clap"), ("cymbals", "cymbal"), ("percussion", "perc")):
        report["drum/" + role] = run(os.path.join(one, "drum", sub), lambda f, r=role: r,
                                     lambda r, f: os.path.join(base, "drum", r, f), None, True)
    report["drum/hats"] = run(os.path.join(one, "drum", "hats"), hat_role,
                              lambda r, f: os.path.join(base, "drum", r, f), None, True)
    # Vocal phrases need a BPM token for varispeed sync (SPEC §9.3); files without one are not copied.
    vocal_dir = os.path.join(one, "vocal")
    bpm_re = re.compile(r"(\d{2,3}(?:\.\d+)?)\s*[-_ ]*bpm|bpm[-_ ]*(\d{2,3})", re.IGNORECASE)
    if os.path.isdir(vocal_dir):
        keep = os.path.join(base, "_vocal_tmp"); os.makedirs(keep, exist_ok=True)
        for f in os.listdir(vocal_dir):
            if f.lower().endswith(".wav") and not bpm_re.search(f):
                report.setdefault("vocal_skipped_nobpm", (0, 0, 0))
                n0, t0, s0 = report["vocal_skipped_nobpm"]; report["vocal_skipped_nobpm"] = (n0, t0, s0 + 1)
    report["vocal"] = run(vocal_dir, lambda f: "vocal",
                          lambda r, f: os.path.join(base, "vocal", key_folder(f) or "NA", f) if bpm_re.search(f) else os.path.join(base, "_vocal_tmp", f), 8000, False)
    shutil.rmtree(os.path.join(base, "_vocal_tmp"), ignore_errors=True)
    report["fx"] = run(os.path.join(one, "fx"), lambda f: "fx", lambda r, f: os.path.join(base, "fx", f), 1500, False)
    for sub in ("bass", "chords", "lead"):
        report["inst/" + sub] = run(os.path.join(one, sub), lambda f: "inst",
                                    lambda r, f, s=sub: os.path.join(base, "inst", s, f), 1000, False)
    return report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--midi", required=True); ap.add_argument("--pack", required=True)
    ap.add_argument("--genre", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=None, help="max files per folder (for quick tests)")
    ap.add_argument("--midi-only", action="store_true")
    args = ap.parse_args()
    for sub in (("midi",) if args.midi_only else ("midi", os.path.join("packs", args.genre))):
        p = os.path.join(args.out, "lp", sub)
        if os.path.isdir(p):
            shutil.rmtree(p)
    print("midi:", import_midi(args.midi, args.out), flush=True)
    if args.midi_only:
        print("done"); return
    for k, (n, total, skipped) in import_pack(args.pack, args.genre, args.out, args.limit).items():
        print(f"{args.genre}/{k:12s} {n:4d} files {total / 1024 / 1024:7.1f} MB  skipped {skipped}", flush=True)
    print("done")

if __name__ == "__main__":
    main()
