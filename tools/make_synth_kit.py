#!/usr/bin/env python3
"""Synthesise the Starter drum kit of the free card bundle (docs/SHARING.md): no sample is taken from anywhere.

Writes 22050 Hz mono 16-bit one-shots into the card layout tools/import_library.py produces, so the firmware's kit
builder (gen/generator.cpp kRole / kFallback) and tools/write_indexes.py / card_qa.py treat the kit like an imported
pack:

  <out>/lp/packs/<Genre>/drum/<role>/<Key>__<Genre>__<Role>__<style>_<nn>.wav

  role    kick snare clap hh oh perc cymbal — the Beat kit takes kick, snare (else clap), hh, oh (else hh), two perc,
          clap (else perc) and cymbal (else perc); the Percussion kit four files from perc + clap + cymbal (SPEC §6)
  Key     a kick carries the key label of the root it is tuned to (A .. G#, sharps only) so the keyed-kick rule
          (Library::pickSample, SPEC §9.7) finds a kick in the project key — 4 per root at the default count; every
          other role is keyless (NA)
  style   the recipe (kick 909ish / 808ish / Punch / Deep, snare Tight / Fat / Bright / Noisy, ...)
  nn      the variant number; sha1("<seed>|<role>|<style>|<root>|<nn>") seeds the parameter draw, so a file is
          reproducible from its name and --seed

Synthesis (stdlib only, a few seconds for the whole kit): kick = sine pitch sweep onto the root (E1 .. D#2) + click +
saturation, 300-400 ms; snare = two detuned tones with a pitch drop + filtered noise; clap = 3-4 noise bursts 9-13 ms
apart + a band-passed tail; hats = a six-oscillator square bank (808 ratios) + noise through a high-pass, closed
60-120 ms, open 260-320 ms; perc = rim / tom / conga / block / cowbell / shaker tones; cymbal = a dense square bank +
noise with a ring of 250-600 ms. Every length respects its slot cap (core/config.h: kick 400, snare 350, hh 120, oh 350,
perc slots 250 ms; a cymbal or clap plays from a perc slot, where the loader cuts it at 250 ms with a 10 ms fade).

Level: every render is peak-normalised to -1 dBFS; then, unless --no-balance, the role balance of tools/import_library.py
(the REAPER drum_balance targets: loudest-20 ms RMS and peak per role, +4 dB trim, never above -0.5 dBFS peak, never a
boost) is applied so the kit sits level with an imported pack (measured House pack: kick peak -7 dBFS, snare -8, clap
-9, hats -13..-14, cymbal -14, perc -12). The firmware applies no per-role gain of its own.

Usage:
  python3 -I tools/make_synth_kit.py --out sdcard/lp_free [--genre Starter] [--seed starter]
          [--counts kick=48,snare=36,clap=30,hh=36,oh=24,perc=48,cymbal=24] [--no-balance] [--json report.json] [--quiet]

The <genre>/drum/<role> folders are rebuilt on every run. Afterwards: tools/write_indexes.py <out> (the folders stay
under 64 files, so no sharding is needed) and tools/card_qa.py <out>.
"""
import argparse
import hashlib
import json
import math
import os
import random
import shutil
import sys
import wave
from array import array

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from import_library import ROLES, TRIM_DB, rms_db_loudest  # noqa: E402  (the imported packs' role balance)

RATE = 22050
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
DEFAULT_COUNTS = {"kick": 48, "snare": 36, "clap": 30, "hh": 36, "oh": 24, "perc": 48, "cymbal": 24}
STYLES = {
    "kick": ["909ish", "808ish", "Punch", "Deep"],
    "snare": ["Tight", "Fat", "Bright", "Noisy"],
    "clap": ["Tight", "Wide", "Room"],
    "hh": ["Metal", "Noise", "Crisp"],
    "oh": ["Metal", "Noise"],
    "perc": ["Rim", "Tom", "Conga", "Block", "Bell", "Shaker"],
    "cymbal": ["Ride", "Crash", "Splash"],
}
ROLE_WORD = {"kick": "Kick", "snare": "Snare", "clap": "Clap", "hh": "HatClosed", "oh": "HatOpen", "perc": "Perc", "cymbal": "Cymbal"}
# the longest file a role may be (ms): the slot cap of core/config.h where the role has a slot of its own, the import
# tool's maximum for the roles that play from a perc slot (clap, cymbal: the loader fades the cut at 250 ms)
MAX_MS = {"kick": 400, "snare": 350, "clap": 250, "hh": 120, "oh": 350, "perc": 250, "cymbal": 600}
TAIL_FADE_MS = {"kick": 12, "snare": 15, "clap": 15, "hh": 8, "oh": 20, "perc": 12, "cymbal": 25}
RATIOS_808 = [1.0, 1.483, 1.8, 2.546, 2.63, 3.897]      # the six hi-hat oscillators of the 808, relative to the lowest
TWO_PI = 2.0 * math.pi


# ----------------------------------------------------------------------------------------------- helpers
def ms(x):
    return max(1, int(RATE * x / 1000.0))


def env_exp(n, tau_ms, floor=0.0, floor_tau_ms=None):
    """exp(-t / tau), optionally over a slower floor (a second, quieter decay)."""
    k = math.exp(-1000.0 / (RATE * tau_ms))
    out = [0.0] * n
    g = 1.0
    if floor > 0.0:
        k2 = math.exp(-1000.0 / (RATE * (floor_tau_ms or tau_ms * 3)))
        g2 = floor
        for i in range(n):
            out[i] = g + g2
            g *= k
            g2 *= k2
    else:
        for i in range(n):
            out[i] = g
            g *= k
    return out


def env_attack(n, attack_ms, tau_ms):
    a = ms(attack_ms)
    out = env_exp(n, tau_ms)
    for i in range(min(a, n)):
        out[i] *= i / a
    return out


def noise(rng, n):
    r = rng.random
    return [r() * 2.0 - 1.0 for _ in range(n)]


def lowpass(x, fc, passes=1):
    a = 1.0 - math.exp(-TWO_PI * fc / RATE)
    for _ in range(passes):
        y = 0.0
        out = []
        for v in x:
            y += a * (v - y)
            out.append(y)
        x = out
    return x


def highpass(x, fc, passes=1):
    for _ in range(passes):
        lp = lowpass(x, fc)
        x = [v - l for v, l in zip(x, lp)]
    return x


def sine_sweep(n, f_start, f_end, tau_ms, phase=0.0):
    """A sine whose frequency falls from f_start to f_end with the time constant tau."""
    k = math.exp(-1000.0 / (RATE * tau_ms))
    f = f_start
    out = []
    for _ in range(n):
        phase += TWO_PI * f / RATE
        out.append(math.sin(phase))
        f = f_end + (f - f_end) * k
    return out


def square_bank(n, freqs, rng):
    """Sum of naive square waves (their aliasing is part of the metal)."""
    out = [0.0] * n
    for f in freqs:
        ph = rng.random()
        inc = f / RATE
        for i in range(n):
            ph += inc
            if ph >= 1.0:
                ph -= 1.0
            out[i] += 1.0 if ph < 0.5 else -1.0
    g = 1.0 / len(freqs)
    return [v * g for v in out]


def saturate(x, drive):
    if drive <= 1.0:
        return x
    norm = math.tanh(drive)
    return [math.tanh(drive * v) / norm for v in x]


def multiply(x, env):
    return [a * b for a, b in zip(x, env)]


def mix(*pairs):
    n = max(len(x) for x, _ in pairs)
    out = [0.0] * n
    for x, g in pairs:
        for i, v in enumerate(x):
            out[i] += g * v
    return out


def finish(x, tail_ms, attack_ms=0.8):
    """Fades (attack, tail), DC block, peak -1 dBFS, int16."""
    n = len(x)
    fi = min(n // 4, ms(attack_ms))
    fo = min(n // 2, ms(tail_ms))
    for i in range(fi):
        x[i] *= i / fi
    for i in range(fo):
        x[n - 1 - i] *= i / fo
    x = highpass(x, 20.0)
    peak = max(abs(v) for v in x) or 1.0
    g = 10 ** (-1.0 / 20) * 32767.0 / peak
    return array("h", [max(-32768, min(32767, int(round(v * g)))) for v in x])


def balance(a, role):
    """tools/import_library.py process(): the role's RMS / peak targets, +4 dB trim, never above -0.5 dBFS, never a boost."""
    _, rms_t, peak_t = ROLES[role]
    peak = max(1, max(abs(v) for v in a)) / 32768.0
    rms = rms_db_loudest(a)
    gain = min(1.0, 10 ** (peak_t / 20) / peak, 10 ** (rms_t / 20) / (10 ** (rms / 20)))
    gain *= 10 ** (TRIM_DB / 20)
    gain = min(gain, 10 ** (-0.5 / 20) / peak)
    return array("h", [max(-32768, min(32767, int(v * gain))) for v in a]), gain


def write_wav(path, a):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(a.tobytes())
    os.replace(tmp, path)


def seed_rng(seed, role, style, root, nn):
    digest = hashlib.sha1(("%s|%s|%s|%s|%d" % (seed, role, style, root, nn)).encode("ascii")).digest()
    return random.Random(int.from_bytes(digest[:8], "big"))


# ----------------------------------------------------------------------------------------------- voices
def synth_kick(rng, style, root):
    U = rng.uniform
    p = 28 + ((root - 4) % 12)                                  # E1 (41 Hz) .. D#2 (62 Hz): the sustain pitch
    f_end = 440.0 * 2 ** ((p - 69) / 12.0)
    if style == "909ish":
        mult, ftau, atau, click, drive, length = U(3.5, 5.0), U(25, 40), U(110, 170), U(0.4, 0.6), U(1.5, 2.0), U(320, 380)
    elif style == "808ish":
        mult, ftau, atau, click, drive, length = U(1.8, 2.6), U(40, 70), U(180, 260), U(0.08, 0.2), U(1.0, 1.3), U(360, 400)
    elif style == "Punch":
        mult, ftau, atau, click, drive, length = U(5.0, 7.0), U(10, 20), U(70, 110), U(0.6, 0.9), U(2.2, 3.0), U(300, 340)
    else:                                                       # Deep
        mult, ftau, atau, click, drive, length = U(2.5, 3.5), U(30, 50), U(150, 220), U(0.2, 0.35), U(1.3, 1.7), U(340, 400)
    n = ms(length)
    body = multiply(sine_sweep(n, f_end * mult, f_end, ftau), env_exp(n, atau))
    h2 = multiply(sine_sweep(n, f_end * mult * 2, f_end * 2, ftau), env_exp(n, atau * 0.25))
    x = mix((body, 1.0), (h2, U(0.08, 0.2)))
    nc = ms(6)
    clk = multiply(highpass(noise(rng, nc), 1500.0), env_exp(nc, U(0.8, 2.0)))
    for i, v in enumerate(clk):
        x[i] += click * v
    x = saturate(x, drive)
    x = lowpass(x, U(5000, 9000))
    return finish(x, TAIL_FADE_MS["kick"])


def synth_snare(rng, style):
    U = rng.uniform
    f1 = U(170, 240)
    ratio = U(1.5, 1.7)
    if style == "Tight":
        ttau, ntau, nfc, tone_g, noise_g, drive, length = U(35, 55), U(45, 80), U(2000, 3500), U(0.5, 0.7), U(0.6, 0.8), U(1.3, 1.8), U(180, 240)
    elif style == "Fat":
        ttau, ntau, nfc, tone_g, noise_g, drive, length = U(60, 100), U(100, 160), U(1200, 2200), U(0.7, 0.9), U(0.5, 0.7), U(1.4, 2.0), U(260, 340)
    elif style == "Bright":
        ttau, ntau, nfc, tone_g, noise_g, drive, length = U(35, 60), U(70, 120), U(3000, 4500), U(0.4, 0.6), U(0.8, 1.0), U(1.2, 1.6), U(200, 280)
    else:                                                       # Noisy
        ttau, ntau, nfc, tone_g, noise_g, drive, length = U(30, 50), U(90, 150), U(1500, 3000), U(0.25, 0.4), 1.0, U(1.1, 1.4), U(220, 320)
    n = ms(length)
    tone = mix((multiply(sine_sweep(n, f1 * 1.3, f1, 18), env_exp(n, ttau)), 1.0),
               (multiply(sine_sweep(n, f1 * ratio * 1.3, f1 * ratio, 18), env_exp(n, ttau * 0.8)), 0.6))
    nz = multiply(highpass(lowpass(noise(rng, n), 9000.0), nfc), env_exp(n, ntau))
    x = saturate(mix((tone, tone_g), (nz, noise_g)), drive)
    return finish(x, TAIL_FADE_MS["snare"])


def synth_clap(rng, style):
    U = rng.uniform
    bursts = rng.choice([3, 4])
    gap = U(9, 13)
    btau = U(5, 9)
    if style == "Tight":
        tail_tau, tail_g, lo, hi, length = U(50, 80), U(0.6, 0.8), U(900, 1300), U(2600, 3800), U(150, 200)
    elif style == "Wide":
        tail_tau, tail_g, lo, hi, length = U(90, 130), U(0.7, 0.9), U(700, 1100), U(2200, 3200), U(200, 250)
    else:                                                       # Room
        tail_tau, tail_g, lo, hi, length = U(120, 160), U(0.8, 1.0), U(800, 1200), U(1800, 2800), U(220, 250)
    n = ms(length)
    base = highpass(lowpass(noise(rng, n), hi, 2), lo, 2)
    env = [0.0] * n
    for b in range(bursts):
        start = ms(b * gap)
        for i, v in enumerate(env_exp(n - start, btau)):
            env[start + i] = max(env[start + i], v)
    start = ms((bursts - 1) * gap)
    for i, v in enumerate(env_exp(n - start, tail_tau)):
        env[start + i] = max(env[start + i], tail_g * v)
    x = saturate(multiply(base, env), U(1.2, 1.8))
    return finish(x, TAIL_FADE_MS["clap"])


def synth_hat(rng, style, open_hat):
    U = rng.uniform
    f0 = U(280, 420)
    if open_hat:
        tau, length, fc = U(70, 130), U(260, 320), U(4500, 6500)
    else:
        tau, length, fc = U(12, 35), U(60, 120), U(5500, 7500)
    if style == "Metal":
        noise_g, bank_g = U(0.2, 0.4), 1.0
    elif style == "Noise":
        noise_g, bank_g = U(0.8, 1.0), U(0.3, 0.5)
    else:                                                       # Crisp
        noise_g, bank_g, fc, tau = 0.5, 0.7, fc + 1000, tau * 0.8
    n = ms(length)
    freqs = [f0 * r * U(0.98, 1.02) for r in RATIOS_808]
    bank = highpass(square_bank(n, freqs, rng), fc, 2)
    nz = highpass(noise(rng, n), fc * 0.9, 2)
    x = mix((bank, bank_g), (nz, noise_g))
    env = env_exp(n, tau, 0.15 if open_hat else 0.0, tau * 2.5)
    x = saturate(multiply(x, env), U(1.0, 1.4))
    return finish(x, TAIL_FADE_MS["oh" if open_hat else "hh"])


def synth_perc(rng, style):
    U = rng.uniform
    if style == "Rim":
        n = ms(U(60, 100))
        f = U(700, 950)
        tone = multiply(sine_sweep(n, f * 1.1, f, 5), env_exp(n, U(8, 15)))
        body = multiply(sine_sweep(n, 450, 380, 8), env_exp(n, U(15, 25)))
        clk = multiply(highpass(noise(rng, n), 2500.0), env_exp(n, U(4, 8)))
        x = saturate(mix((tone, 0.8), (body, 0.5), (clk, 0.6)), 2.0)
    elif style == "Tom":
        n = ms(U(180, 250))
        f = U(140, 200)
        x = multiply(sine_sweep(n, f * 1.6, f * 0.85, U(40, 70)), env_exp(n, U(70, 110)))
        clk = multiply(highpass(noise(rng, n), 1500.0), env_exp(n, 3))
        x = lowpass(saturate(mix((x, 1.0), (clk, 0.3)), 1.5), 6000.0)
    elif style == "Conga":
        n = ms(U(140, 220))
        f = U(190, 280)
        x = multiply(sine_sweep(n, f * 1.2, f, U(10, 20)), env_exp(n, U(60, 90)))
        h2 = multiply(sine_sweep(n, f * 2.4, f * 2, U(10, 20)), env_exp(n, U(20, 30)))
        clk = multiply(highpass(noise(rng, n), 2000.0), env_exp(n, 4))
        x = saturate(mix((x, 1.0), (h2, 0.15), (clk, 0.25)), 1.4)
    elif style == "Block":
        n = ms(U(50, 80))
        f = U(800, 1000)
        x = mix((multiply(sine_sweep(n, f, f, 10), env_exp(n, U(15, 25))), 1.0),
                (multiply(sine_sweep(n, f * 1.5, f * 1.5, 10), env_exp(n, 10)), 0.5),
                (multiply(highpass(noise(rng, n), 3000.0), env_exp(n, 3)), 0.4))
        x = saturate(x, 1.3)
    elif style == "Bell":                                       # cowbell: two squares a fifth-ish apart
        n = ms(U(130, 200))
        f = U(500, 580)
        bank = lowpass(square_bank(n, [f, f * 1.48], rng), 4000.0, 2)
        clk = multiply(highpass(noise(rng, n), 3000.0), env_exp(n, 3))
        x = saturate(mix((multiply(bank, env_exp(n, U(50, 90))), 1.0), (clk, 0.3)), 1.6)
    else:                                                       # Shaker
        n = ms(U(80, 140))
        nz = highpass(noise(rng, n), U(4000, 6000), 2)
        x = multiply(nz, env_attack(n, U(5, 15), U(25, 55)))
    return finish(x, TAIL_FADE_MS["perc"])


def synth_cymbal(rng, style):
    U = rng.uniform
    if style == "Ride":
        n, tau, fc = ms(U(450, 600)), U(160, 240), U(3500, 5000)
        attack = 1.0
    elif style == "Crash":
        n, tau, fc = ms(U(400, 550)), U(140, 200), U(2500, 3500)
        attack = 3.0
    else:                                                       # Splash
        n, tau, fc = ms(U(250, 350)), U(70, 120), U(4500, 5500)
        attack = 1.5
    f0 = U(480, 720)
    freqs = [f0 * r * U(0.97, 1.03) for r in RATIOS_808 + [4.2, 5.3]]
    bank = highpass(square_bank(n, freqs, rng), fc, 2)
    nz = highpass(noise(rng, n), fc * 0.9, 2)
    ping = multiply(sine_sweep(n, U(2200, 3200), U(2200, 3200), 50), env_exp(n, 60))
    x = mix((bank, 0.6 if style == "Ride" else 0.5), (nz, 0.5 if style == "Ride" else 1.0), (ping, 0.25 if style == "Ride" else 0.1))
    env = env_attack(n, attack, tau)
    floor = env_exp(n, tau * 3)
    env = [e + 0.08 * f for e, f in zip(env, floor)]
    x = saturate(multiply(x, env), 1.2 if style == "Ride" else 1.3)
    return finish(x, TAIL_FADE_MS["cymbal"])


# ----------------------------------------------------------------------------------------------- kit
def variants(role, count):
    """(style, root, nn) of every file of a role: kicks cycle the 12 roots, then the styles; other roles the styles."""
    styles = STYLES[role]
    out = []
    for i in range(count):
        if role == "kick":
            root = i % 12
            style = styles[(i // 12) % len(styles)]
            nn = i // 12 + 1
        else:
            root = None
            style = styles[i % len(styles)]
            nn = i // len(styles) + 1
        out.append((style, root, nn))
    return out


def render(role, style, root, rng):
    if role == "kick":
        return synth_kick(rng, style, root)
    if role == "snare":
        return synth_snare(rng, style)
    if role == "clap":
        return synth_clap(rng, style)
    if role in ("hh", "oh"):
        return synth_hat(rng, style, role == "oh")
    if role == "perc":
        return synth_perc(rng, style)
    return synth_cymbal(rng, style)


def db(x):
    return 20 * math.log10(max(x, 1e-6))


def make_kit(out_root, genre, seed, counts, do_balance, quiet):
    base = os.path.join(out_root, "lp", "packs", genre, "drum")
    report = {}
    for role, count in counts.items():
        folder = os.path.join(base, role)
        if os.path.isdir(folder):
            shutil.rmtree(folder)
        os.makedirs(folder, exist_ok=True)
        lens, peaks, rmss, gains = [], [], [], []
        names = set()
        for style, root, nn in variants(role, count):
            rng = seed_rng(seed, role, style, root if root is not None else "NA", nn)
            a = render(role, style, root, rng)
            if len(a) > MAX_MS[role] * RATE // 1000:
                a = a[: MAX_MS[role] * RATE // 1000]
            gain = 1.0
            if do_balance:
                a, gain = balance(a, role)
            label = NOTE_NAMES[root] if root is not None else "NA"
            name = "%s__%s__%s__%s_%02d.wav" % (label, genre, ROLE_WORD[role], style, nn)
            if name in names:
                raise SystemExit("duplicate name " + name)
            names.add(name)
            write_wav(os.path.join(folder, name), a)
            lens.append(len(a) * 1000.0 / RATE)
            peaks.append(db(max(abs(v) for v in a) / 32768.0))
            rmss.append(rms_db_loudest(a))
            gains.append(db(gain))
        rep = dict(files=count, ms_min=round(min(lens)), ms_max=round(max(lens)),
                   peak_dbfs_min=round(min(peaks), 1), peak_dbfs_max=round(max(peaks), 1),
                   rms20ms_db_min=round(min(rmss), 1), rms20ms_db_max=round(max(rmss), 1),
                   balance_db_min=round(min(gains), 1), balance_db_max=round(max(gains), 1),
                   bytes=sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder)))
        report[role] = rep
        if not quiet:
            print("%-6s %3d files  %3d-%3d ms  peak %5.1f..%5.1f dBFS  rms20 %5.1f..%5.1f dB  balance %5.1f..%5.1f dB  %6.1f KB"
                  % (role, count, rep["ms_min"], rep["ms_max"], rep["peak_dbfs_min"], rep["peak_dbfs_max"], rep["rms20ms_db_min"],
                     rep["rms20ms_db_max"], rep["balance_db_min"], rep["balance_db_max"], rep["bytes"] / 1024))
    return report


def parse_counts(text):
    counts = dict(DEFAULT_COUNTS)
    if text:
        for part in text.split(","):
            role, n = part.split("=")
            if role not in DEFAULT_COUNTS:
                sys.exit("unknown role %r (%s)" % (role, " ".join(DEFAULT_COUNTS)))
            counts[role] = int(n)
    return {r: n for r, n in counts.items() if n > 0}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="card root: files go to <out>/lp/packs/<genre>/drum/<role>/")
    ap.add_argument("--genre", default="Starter")
    ap.add_argument("--seed", default="starter", help="base seed of every parameter draw (default: starter)")
    ap.add_argument("--counts", help="files per role, e.g. kick=48,snare=36 (defaults: %s)" % ",".join("%s=%d" % kv for kv in DEFAULT_COUNTS.items()))
    ap.add_argument("--no-balance", action="store_true", help="keep every file at -1 dBFS peak instead of the imported packs' role balance")
    ap.add_argument("--json", help="write the per-role report here")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    counts = parse_counts(args.counts)
    report = dict(out=os.path.abspath(args.out), genre=args.genre, seed=args.seed, balance=not args.no_balance,
                  roles=make_kit(os.path.abspath(args.out), args.genre, args.seed, counts, not args.no_balance, args.quiet))
    total = sum(r["files"] for r in report["roles"].values())
    nbytes = sum(r["bytes"] for r in report["roles"].values())
    print("%d files, %.2f MB -> %s/lp/packs/%s/drum" % (total, nbytes / 1e6, os.path.abspath(args.out), args.genre))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
