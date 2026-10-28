#!/usr/bin/env python3
"""Build the Live Player SD-card content tree from downloaded test material.

Usage:
  python3 -I prepare_content.py --downloads <dir> --out <sdcard dir> [--seed N]

Inputs (each in its own folder under --downloads):
  free-midi-chords/    ldrolez/free-midi-chords release (MIT)
  groove-midi/groove/  Google Magenta Groove MIDI Dataset v1.0.0 midi-only (CC BY 4.0)
  sonic-pi-samples/    sparse clone of sonic-pi repo, etc/samples (CC0)
  dirt-samples/repo/   sparse clone of tidalcycles/Dirt-Samples (no license file)

Output layout (SD card root):
  lp/midi/<category>/<Key>/<Key>__<name>.mid  (only with --with-midi) Key = Amin | C | F#min | A# | NA
  lp/packs/<Genre>/{drum/<role>,vocal/NA,fx,inst/misc}/<name>.wav  22050 Hz mono 16-bit PCM
  lp/README.txt                             sources and licenses

MIDI conventions written here match the REAPER Live Player key.lua contract:
  <Key>__ prefix, no "MIN" means major, "NA__" means keyless.
Drum/percussion/vocal/fx MIDI uses note 36..39 for voice A..D
(drums: 36 kick, 37 snare, 38 closed hat, 39 open hat).
"""
import argparse, csv, math, os, random, re, shutil, struct, subprocess, sys, wave
from array import array
from collections import defaultdict

try:
    import mido
except ImportError:
    sys.exit("mido is required: ./.venv/bin/pip install mido")

TPB = 96  # ticks per quarter note written to the SD card files (small, exact 16ths/32nds/triplets)
RATE = 22050
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLAT_TO_SHARP = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#", "Cb": "B", "Fb": "E"}

# ----------------------------------------------------------------------------- helpers

def key_label(root_name, minor):
    """REAPER contract label: 'Amin', 'C', 'F#min'. Flats become sharps."""
    root_name = FLAT_TO_SHARP.get(root_name, root_name)
    return root_name + ("min" if minor else "")

def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9#\-]+", "_", s).strip("_")
    return s[:48]

def write_midi(path, notes, bpm=None, length_beats=None):
    """notes: list of (start_beat, duration_beats, pitch, velocity). Writes a type-0 file."""
    mid = mido.MidiFile(ticks_per_beat=TPB, type=0)
    tr = mido.MidiTrack(); mid.tracks.append(tr)
    if bpm:
        tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    events = []
    for start, dur, pitch, vel in notes:
        s = int(round(start * TPB)); e = max(s + 1, int(round((start + dur) * TPB)))
        events.append((s, 1, pitch, max(1, min(127, int(vel)))))
        events.append((e, 0, pitch, 0))
    # sort: time, note-offs before note-ons at the same tick
    events.sort(key=lambda ev: (ev[0], ev[1]))
    t = 0
    for tick, on, pitch, vel in events:
        tr.append(mido.Message("note_on" if on else "note_off", note=pitch, velocity=vel, time=tick - t))
        t = tick
    end_tick = int(round((length_beats or (max(ev[0] for ev in events) / TPB if events else 4)) * TPB))
    tr.append(mido.MetaMessage("end_of_track", time=max(0, end_tick - t)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mid.save(path)

def read_notes(path):
    """Return (notes, bpm, length_beats) with notes as (start_beat, dur_beats, pitch, vel)."""
    m = mido.MidiFile(path)
    tpb = m.ticks_per_beat
    bpm = None
    pending = {}
    notes = []
    end = 0
    for tr in m.tracks:
        t = 0
        for msg in tr:
            t += msg.time
            end = max(end, t)
            if msg.type == "set_tempo" and bpm is None:
                bpm = mido.tempo2bpm(msg.tempo)
            if msg.type == "note_on" and msg.velocity > 0:
                pending.setdefault(msg.note, []).append((t, msg.velocity))
            elif msg.type == "note_off" or (msg.type == "note_on" and msg.velocity == 0):
                if pending.get(msg.note):
                    s, v = pending[msg.note].pop(0)
                    notes.append((s / tpb, max(1 / 32, (t - s) / tpb), msg.note, v))
    notes.sort()
    return notes, bpm, end / tpb

# ----------------------------------------------------------------------------- chords

KEY_DIR_RE = re.compile(r"^\d\d - (\w+) Major - (\w+#?) minor$")

def chord_sources(root):
    """Yield (major_root, minor_root, mode, style, path) for every progression in the chords pack."""
    base = os.path.join(root, "free-midi-chords", "chords")
    for key_dir in sorted(os.listdir(base)):
        mm = KEY_DIR_RE.match(key_dir)
        if not mm:
            continue
        major_root, minor_root = mm.group(1), mm.group(2)
        prog = os.path.join(base, key_dir, "4 Progression")
        if not os.path.isdir(prog):
            continue
        for mode in ("Major", "Minor"):          # Modal folder skipped: not strictly in key
            mode_dir = os.path.join(prog, mode)
            if not os.path.isdir(mode_dir):
                continue
            for style in ("straight", "pop", "pop2", "hiphop2", "soul"):
                d = mode_dir if style == "straight" else os.path.join(mode_dir, style)
                if not os.path.isdir(d):
                    continue
                for f in sorted(os.listdir(d)):
                    if f.lower().endswith(".mid"):
                        yield major_root, minor_root, mode, style, os.path.join(d, f)

def chord_tones_per_onset(notes):
    """Group notes by onset -> list of (start, dur, sorted pitches, vel)."""
    groups = defaultdict(list)
    for s, d, p, v in notes:
        groups[round(s, 4)].append((d, p, v))
    out = []
    for s in sorted(groups):
        g = groups[s]
        out.append((s, max(d for d, _, _ in g), sorted(p for _, p, _ in g), g[0][2]))
    return out

def transpose_to_range(pitch, lo, hi):
    while pitch < lo: pitch += 12
    while pitch > hi: pitch -= 12
    return pitch

def make_bass(groups, length, style_rng):
    """Derive a bass line from chord roots. Several rhythmic styles."""
    style = style_rng.choice(["root8", "offbeat", "octave16", "fourfloor", "rolling"])
    out = []
    for i, (s, d, pitches, vel) in enumerate(groups):
        nxt = groups[i + 1][0] if i + 1 < len(groups) else length
        seg = nxt - s
        root = transpose_to_range(min(pitches), 36, 47)     # C2..B2
        fifth = root + 7 if root + 7 <= 50 else root - 5
        t = 0.0
        while t < seg - 1e-6:
            if style == "root8":
                out.append((s + t, 0.45, root, 100 if t % 1 == 0 else 84)); t += 0.5
            elif style == "offbeat":
                out.append((s + t + 0.5, 0.4, root, 96)); t += 1.0
            elif style == "octave16":
                for k in range(4):
                    p = root if k % 2 == 0 else root + 12
                    out.append((s + t + k * 0.25, 0.2, p, 92 if k == 0 else 78))
                t += 1.0
            elif style == "fourfloor":
                out.append((s + t, 0.9, root, 104)); t += 1.0
            else:  # rolling root/fifth
                out.append((s + t, 0.45, root, 100)); out.append((s + t + 0.5, 0.4, fifth if (t // 1) % 2 else root, 80)); t += 1.0
    out = [(st, du, p, v) for st, du, p, v in out if st < length - 1e-6]
    return style, out

SCALE_MAJOR = [0, 2, 4, 5, 7, 9, 11]
SCALE_MINOR = [0, 2, 3, 5, 7, 8, 10]

def scale_pitches(root_pc, minor, lo, hi):
    ints = SCALE_MINOR if minor else SCALE_MAJOR
    return [p for p in range(lo, hi + 1) if (p - root_pc) % 12 in ints]

def make_lead(groups, length, root_pc, minor, style_rng):
    """Derive a lead from chord tones: arpeggios, top-note melodies with scale passing notes."""
    style = style_rng.choice(["arp_up8", "arp_updown16", "top_melody", "stab_syncopated", "arp_down8"])
    scale = scale_pitches(root_pc, minor, 60, 84)
    out = []
    prev = None
    for i, (s, d, pitches, vel) in enumerate(groups):
        nxt = groups[i + 1][0] if i + 1 < len(groups) else length
        seg = nxt - s
        tones = sorted(set(transpose_to_range(p, 60, 79) for p in pitches if p >= 48))
        if not tones:
            tones = sorted(set(transpose_to_range(p, 60, 79) for p in pitches))
        t = 0.0; k = 0
        while t < seg - 1e-6:
            if style == "arp_up8":
                p = tones[k % len(tones)]; out.append((s + t, 0.45, p, 96 if k % 4 == 0 else 80)); t += 0.5; k += 1
            elif style == "arp_down8":
                p = tones[(-k) % len(tones)]; out.append((s + t, 0.45, p, 96 if k % 4 == 0 else 80)); t += 0.5; k += 1
            elif style == "arp_updown16":
                seq = tones + tones[-2:0:-1] if len(tones) > 2 else tones
                p = seq[k % len(seq)]; out.append((s + t, 0.22, p, 92 if k % 4 == 0 else 74)); t += 0.25; k += 1
            elif style == "stab_syncopated":
                for off in (0.0, 0.75, 1.5, 2.25, 3.0, 3.5):
                    if off < seg:
                        p = tones[(k + int(off * 2)) % len(tones)]
                        out.append((s + off, 0.3, p, 100 if off == 0 else 86))
                t = seg; k += 1
            else:  # top_melody: chord top note held, with passing notes toward next chord
                top = tones[-1]
                out.append((s + t, 1.4, top, 100))
                nxt_top = (sorted(set(transpose_to_range(p, 60, 79) for p in groups[i + 1][2] if p >= 48)) or [top])[-1] if i + 1 < len(groups) else top
                step_pool = [p for p in scale if min(top, nxt_top) <= p <= max(top, nxt_top)] or [top]
                t2 = t + 1.5
                while t2 < seg - 0.4:
                    out.append((s + t2, 0.45, style_rng.choice(step_pool), 72)); t2 += 0.5
                t = seg
    out = [(st, du, p, v) for st, du, p, v in out if st < length - 1e-6]
    return style, out

def make_pad(groups, length):
    """Sustained voicing per chord, no bass note, legato."""
    out = []
    for i, (s, d, pitches, vel) in enumerate(groups):
        nxt = groups[i + 1][0] if i + 1 < len(groups) else length
        for p in sorted(set(transpose_to_range(p, 55, 76) for p in pitches if p >= 48))[:4]:
            out.append((s, nxt - s - 0.05, p, 80))
    return out

def build_tonal(root, out, rng, limits):
    sources = list(chord_sources(root))
    rng.shuffle(sources)
    per_key_count = defaultdict(int)
    written = defaultdict(int)
    for major_root, minor_root, mode, style, path in sources:
        minor = mode == "Minor"
        root_name = minor_root if minor else major_root
        label = key_label(root_name, minor)
        if per_key_count[label] >= limits["per_key"]:
            continue
        notes, bpm, length = read_notes(path)
        if not notes:
            continue
        length = max(4.0, math.ceil(length / 4) * 4)       # whole bars
        if length > 16:
            continue
        groups = chord_tones_per_onset(notes)
        base = safe_name(os.path.splitext(os.path.basename(path))[0].split(" - ", 1)[-1])
        name = f"{label}__{base}_{style}"
        per_key_count[label] += 1
        # chord: original voicing, straight from the pack (bass note dropped: that is the bass track's job)
        chord_notes = [(s, d, p, v) for s, d, p, v in notes if p >= 48] or notes
        write_midi(os.path.join(out, "lp", "midi", "Chord", label, name + ".mid"), chord_notes, length_beats=length); written["chord"] += 1
        if style == "straight":   # derived material only from the straight versions
            root_pc = NOTE_NAMES.index(FLAT_TO_SHARP.get(root_name, root_name))
            bstyle, bass = make_bass(groups, length, rng)
            write_midi(os.path.join(out, "lp", "midi", "Bass", label, f"{label}__{base}_{bstyle}.mid"), bass, length_beats=length); written["bass"] += 1
            lstyle, lead = make_lead(groups, length, root_pc, minor, rng)
            write_midi(os.path.join(out, "lp", "midi", "Lead", label, f"{label}__{base}_{lstyle}.mid"), lead, length_beats=length); written["lead"] += 1
            write_midi(os.path.join(out, "lp", "midi", "Pad", label, f"{label}__{base}_pad.mid"), make_pad(groups, length), length_beats=length); written["pad"] += 1
    return written

# ----------------------------------------------------------------------------- drums (Groove MIDI Dataset)

# Roland TD-11 pitches (Groove MIDI Dataset) -> Live Player drum voices
DRUM_MAP = {36: 36, 38: 37, 37: 37, 40: 37, 42: 38, 44: 38, 22: 38, 46: 39, 26: 39}
# toms / cymbals / rims -> four percussion voices A..D
PERC_MAP = {43: 36, 45: 36, 47: 37, 48: 37, 50: 37, 51: 38, 53: 38, 59: 38, 49: 39, 55: 39, 57: 39, 52: 39}

def quantize(x, grid=0.25):
    return round(x / grid) * grid

def slice_groove(path, mapping, bars=2, min_hits=8, required=(36, 37, 38), max_slices=3):
    notes, bpm, length = read_notes(path)
    beats = bars * 4
    hits = [(quantize(s), mapping[p], v) for s, d, p, v in notes if p in mapping]
    if not hits:
        return []
    total_bars = int(length // 4)
    slices = []
    for b in range(0, max(0, total_bars - bars + 1), bars):
        start = b * 4
        seg = [((s - start), p, v) for s, p, v in hits if start <= s < start + beats]
        voices = {p for _, p, _ in seg}
        if len(seg) < min_hits or not all(r in voices for r in required):
            continue
        # dedupe (same voice, same tick)
        seen = {}
        for s, p, v in seg:
            seen[(s, p)] = max(v, seen.get((s, p), 0))
        slices.append([(s, 0.125, p, v) for (s, p), v in sorted(seen.items())])
        if len(slices) >= max_slices:
            break
    return slices

def build_drums(root, out, rng, limits):
    base = os.path.join(root, "groove-midi", "groove")
    rows = list(csv.DictReader(open(os.path.join(base, "info.csv"))))
    rows = [r for r in rows if r["beat_type"] == "beat" and r["time_signature"] == "4-4"]
    rng.shuffle(rows)
    counts = defaultdict(int)
    per_style = defaultdict(int)
    for r in rows:
        style = r["style"].split("/")[0]
        if per_style[style] >= limits["drum_per_style"]:
            continue
        path = os.path.join(base, r["midi_filename"])
        for i, sl in enumerate(slice_groove(path, DRUM_MAP)):
            name = f"NA__{style}_{r['bpm']}bpm_{r['id'].replace('/', '_')}_{i + 1}.mid"
            write_midi(os.path.join(out, "lp", "midi", "Beat", "NA", name), sl, bpm=int(r["bpm"]), length_beats=8)
            counts["drum"] += 1; per_style[style] += 1
        for i, sl in enumerate(slice_groove(path, PERC_MAP, min_hits=6, required=(36,), max_slices=1)):
            name = f"NA__{style}_{r['bpm']}bpm_{r['id'].replace('/', '_')}_perc{i + 1}.mid"
            write_midi(os.path.join(out, "lp", "midi", "Percussion", "NA", name), sl, bpm=int(r["bpm"]), length_beats=8)
            counts["percussion"] += 1
    return counts

# ----------------------------------------------------------------------------- generated rhythmic patterns (NA)

def euclid(k, n, rot=0):
    pat = []
    bucket = 0
    for i in range(n):
        bucket += k
        if bucket >= n:
            bucket -= n; pat.append(1)
        else:
            pat.append(0)
    return pat[rot:] + pat[:rot]

def build_generated_rhythms(out, rng, n_perc=24, n_vocal=24, n_fx=24):
    counts = defaultdict(int)
    for i in range(n_perc):
        notes = []
        for voice in range(4):
            k = rng.choice([2, 3, 4, 5, 6, 7]); rot = rng.randrange(16)
            for step, on in enumerate(euclid(k, 16, rot)):
                if on and rng.random() < 0.9:
                    notes.append((step * 0.25, 0.125, 36 + voice, rng.randint(70, 110)))
        write_midi(os.path.join(out, "lp", "midi", "Percussion", "NA", f"NA__euclid_{i + 1:02d}.mid"), notes, length_beats=4)
        counts["percussion"] += 1
    for i in range(n_vocal):
        notes = []
        n_hits = rng.choice([1, 2, 2, 3, 4])
        slots = sorted(rng.sample([0, 1, 1.5, 2, 2.5, 3, 3.5, 4, 5, 6, 6.5, 7, 7.5], n_hits))
        for s in slots:
            notes.append((s, 0.5, 36 + rng.randrange(4), rng.randint(90, 115)))
        write_midi(os.path.join(out, "lp", "midi", "Vocal", "NA", f"NA__chops_{i + 1:02d}.mid"), notes, length_beats=8)
        counts["vocal"] += 1
    for i in range(n_fx):
        notes = []
        kind = rng.choice(["sparse", "downbeat", "fill"])
        if kind == "sparse":
            for s in rng.sample([0, 3.5, 7, 7.5, 11, 14, 15, 15.5], rng.choice([1, 2, 3])):
                notes.append((s, 0.5, 48 + rng.randrange(4), 100))
        elif kind == "downbeat":
            notes.append((0, 0.5, 48 + rng.randrange(4), 110))
            if rng.random() < 0.5: notes.append((8, 0.5, 48 + rng.randrange(4), 100))
        else:
            for k in range(4):
                notes.append((14 + k * 0.5, 0.25, 48 + rng.randrange(4), 80 + k * 10))
        write_midi(os.path.join(out, "lp", "midi", "Fx", "NA", f"NA__{kind}_{i + 1:02d}.mid"), notes, length_beats=16)
        counts["fx"] += 1
    return counts

# ----------------------------------------------------------------------------- samples

def ffmpeg_to_pcm(src, max_seconds):
    """Decode any audio file to 22050 Hz mono int16 via ffmpeg; returns array('h')."""
    cmd = ["ffmpeg", "-v", "error", "-i", src, "-t", str(max_seconds), "-ac", "1", "-ar", str(RATE),
           "-f", "s16le", "-acodec", "pcm_s16le", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    a = array("h"); a.frombytes(data[: len(data) - len(data) % 2])
    return a

def process_sample(src, dst, max_ms, fade_ms=12, peak_db=-1.0, trim_db=-48.0):
    a = ffmpeg_to_pcm(src, max_ms / 1000 + 0.5)
    if len(a) < RATE * 0.005:
        return None
    thr = int(32767 * 10 ** (trim_db / 20))
    start = 0
    while start < len(a) and abs(a[start]) < thr:
        start += 1
    start = max(0, start - int(RATE * 0.002))
    a = a[start:]
    # cut at max length, then trim trailing silence
    a = a[: int(RATE * max_ms / 1000)]
    end = len(a)
    while end > 0 and abs(a[end - 1]) < thr // 2:
        end -= 1
    a = a[: max(end, int(RATE * 0.01))]
    peak = max(1, max(abs(x) for x in a))
    gain = (32767 * 10 ** (peak_db / 20)) / peak
    n = len(a); fade = min(n // 2, int(RATE * fade_ms / 1000))
    for i in range(n):
        g = gain
        if i >= n - fade:
            g *= (n - i) / fade
        if i < int(RATE * 0.001):
            g *= i / (RATE * 0.001)
        v = int(a[i] * g)
        a[i] = max(-32768, min(32767, v))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with wave.open(dst, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(a.tobytes())
    return len(a) * 2

def collect(folder, exts=(".wav", ".flac", ".aif", ".aiff")):
    out = []
    for dp, _, fs in os.walk(folder):
        for f in fs:
            if f.lower().endswith(exts):
                out.append(os.path.join(dp, f))
    return sorted(out)

def build_samples(root, out, rng, limits):
    sp = os.path.join(root, "sonic-pi-samples", "repo", "etc", "samples")
    dirt = os.path.join(root, "dirt-samples", "repo")
    sp_files = {os.path.splitext(os.path.basename(f))[0]: f for f in collect(sp)}

    def sp_pick(prefixes, exclude=()):
        return [f for n, f in sorted(sp_files.items()) if n.startswith(prefixes) and not n.startswith(exclude)]

    def dirt_pick(folders, limit_per_folder=None):
        out_files = []
        for folder in folders:
            fs = collect(os.path.join(dirt, folder))
            if limit_per_folder:
                rng.shuffle(fs); fs = fs[:limit_per_folder]
            out_files += [(folder, f) for f in fs]
        return out_files

    plan = {
        # category/role: (max_ms, [(label, path), ...])
        "drum/kick":  (350, [("sp", f) for f in sp_pick(("bd_", "drum_bass_", "drum_heavy_kick", "elec_hollow_kick", "elec_soft_kick"))]
                             + dirt_pick(["808bd", "clubkick", "hardkick", "popkick"], 4)),
        "drum/snare": (300, [("sp", f) for f in sp_pick(("sn_", "drum_snare_", "elec_snare", "elec_hi_snare", "elec_lo_snare", "elec_mid_snare", "elec_filt_snare"))]
                             + dirt_pick(["808sd", "sn", "cp", "realclaps"], 5)),
        "drum/hh":    (150, [("sp", f) for f in sp_pick(("drum_cymbal_closed", "drum_cymbal_pedal", "hat_", "elec_tick"))]
                             + dirt_pick(["808hc", "hh", "linnhats"], 5)),
        "drum/oh":    (400, [("sp", f) for f in sp_pick(("drum_cymbal_open", "drum_cymbal_soft", "elec_cymbal"))]
                             + dirt_pick(["808oh"], 5)),
        "percussion": (250, [("sp", f) for f in sp_pick(("perc_", "tabla_", "elec_", "drum_cowbell", "drum_tom_", "glitch_perc", "tbd_perc"), exclude=("elec_snare", "elec_hi_snare", "elec_lo_snare", "elec_mid_snare", "elec_filt_snare", "elec_hollow_kick", "elec_soft_kick", "elec_cymbal"))]
                             + dirt_pick(["tabla", "tabla2", "perc", "cb", "clak", "click", "coins", "glasstap", "bottle", "can", "pebbles", "tink", "tok", "latibro", "east", "rm", "hand", "drumtraks", "dr55"], 4)),
        "vocal":      (600, dirt_pick(["yeah", "miniyeah", "speech", "numbers", "alphabet", "mouth", "breath", "hmm", "diphone", "ulgab"], 8)),
        "fx":         (600, [("sp", f) for f in sp_pick(("ambi_", "misc_", "vinyl_", "glitch_robot", "mehackit_", "elec_twang", "elec_fuzz_tom"))]
                             + dirt_pick(["stab", "hoover", "bleep", "blip", "fire", "space", "cosmicg", "incoming", "invaders", "glitch", "ades", "noise", "wind", "feelfx", "bend"], 4)),
        # single-note instrument one-shots (root C where known) for an optional pitched-sampler mode
        "inst":       (800, [("sp", f) for f in sp_pick(("bass_", "tbd_highkey_c4", "tbd_pad_", "ambi_piano", "elec_bell", "elec_chime", "elec_triangle", "elec_bong", "guit_"))]),
    }
    report = {}
    for cat, (max_ms, files) in plan.items():
        total = 0; n = 0; skipped = 0
        for label, src in files:
            stem = safe_name(os.path.splitext(os.path.basename(src))[0]).lower()
            if label != "sp":
                stem = f"{label}_{stem}"
            pack = os.path.join(out, "lp", "packs", limits["genre"])
            if cat.startswith("drum"):
                dst = os.path.join(pack, "drum", cat.split("/")[1], stem + ".wav")
            elif cat == "percussion":
                dst = os.path.join(pack, "drum", "perc", stem + ".wav")
            elif cat == "vocal":
                dst = os.path.join(pack, "vocal", "NA", stem + ".wav")
            elif cat == "inst":
                dst = os.path.join(pack, "inst", "misc", stem + ".wav")
            else:
                dst = os.path.join(pack, cat, stem + ".wav")
            try:
                size = process_sample(src, dst, max_ms)
            except subprocess.CalledProcessError:
                size = None
            if size:
                total += size; n += 1
            else:
                skipped += 1
        report[cat] = (n, total, skipped)
    return report

# ----------------------------------------------------------------------------- main

README = """Live Player test content for the Cardputer
==========================================

Layout
  lp/<category>/midi/<Key>/<Key>__<name>.mid   patterns in a subfolder per key; Key = Amin, C, F#min, A# ... or NA (keyless)
  lp/<category>/samples/<name>.wav       one-shots, 22050 Hz mono 16-bit PCM, peak -1 dBFS
  lp/drum/samples/{kick,snare,hh,oh}/    drum roles; MIDI notes 36 kick, 37 snare, 38 closed hat, 39 open hat
  lp/percussion, lp/vocal, lp/fx         four random one-shots per generated pattern, MIDI notes 36..39 (fx: 48..51)
  lp/inst/samples                        single-note one-shots (mostly root C) for an optional pitched-sampler mode

Key convention follows the REAPER Live Player key.lua contract: text before "__" is the key,
"min" marks minor, no suffix means major, "NA__" means key-independent.

Sources and licenses
  Chord progressions: ldrolez/free-midi-chords v0.20260314, MIT.  Bass, lead and pad files are
    DERIVED from those progressions by tools/prepare_content.py (test material, not hand-made).
  Drum and percussion patterns: Google Magenta Groove MIDI Dataset v1.0.0, CC BY 4.0
    (Gillick et al., "Learning to Groove with Inverse Sequence Transformations", ICML 2019),
    sliced to 2 bars and mapped to the four-voice contract. Euclidean/chop/fx patterns are generated.
  Samples prefixed by their Sonic Pi name (bd_, sn_, drum_, perc_, tabla_, elec_, ambi_, ...):
    sonic-pi etc/samples, CC0 (freesound.org contributors, Arovane, The Black Dog).
  Samples prefixed by a Dirt-Samples folder name (808bd_, yeah_, tabla2_, stab_, ...):
    tidalcycles/Dirt-Samples. The repository states no license; private testing only.
"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--downloads", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=20261014)
    ap.add_argument("--per-key", type=int, default=12, help="chord progressions kept per key label")
    ap.add_argument("--drum-per-style", type=int, default=10)
    ap.add_argument("--skip-samples", action="store_true")
    ap.add_argument("--with-midi", action="store_true", help="also write the derived test MIDI into lp/midi (off by default: the real library lives there)")
    ap.add_argument("--genre", default="Testpack")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = args.out
    lp = os.path.join(out, "lp")
    pack_dir = os.path.join(lp, "packs", args.genre)
    if os.path.isdir(pack_dir):
        shutil.rmtree(pack_dir)
    os.makedirs(os.path.join(pack_dir, "sets"), exist_ok=True)
    limits = {"per_key": args.per_key, "drum_per_style": args.drum_per_style, "genre": args.genre}
    if args.with_midi:
        print("tonal:", dict(build_tonal(args.downloads, out, rng, limits)))
        print("drums:", dict(build_drums(args.downloads, out, rng, limits)))
        print("generated:", dict(build_generated_rhythms(out, rng)))
    if not args.skip_samples:
        for cat, (n, total, skipped) in build_samples(args.downloads, out, rng, limits).items():
            print(f"samples {cat:12s} {n:4d} files {total / 1024:8.1f} KB  skipped {skipped}")
    with open(os.path.join(pack_dir, "README.txt"), "w") as f:
        f.write(README)
    print("done ->", lp)

if __name__ == "__main__":
    main()
