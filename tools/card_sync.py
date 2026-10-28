#!/usr/bin/env python3
"""Sync a local folder onto the Cardputer's microSD through the card loader firmware (serial, no disk mount).
Usage: card_sync.py PORT SRC_DIR [--dest /lp] [--prune] [--verify N] [--exclude rel/path ...]
  SRC_DIR is the local folder that becomes DEST on the card (default: sdcard/lp -> /lp).
  Files whose size already matches on the card are skipped. presets/, sets/ and state/ folders are the
  card's own (saved sets and the session state, SPEC 9.8 / 9.18): they are never copied from SRC_DIR (a host
  test run leaves its own session files in sdcard/lp/state) and never pruned. --prune deletes other card files
  missing locally. --verify N checks the CRC32 of N random copied files afterwards."""
import os, random, sys, time, zlib, serial

def main():
    args = sys.argv[1:]
    if len(args) < 2: print(__doc__); sys.exit(2)
    port, src = args[0], args[1]; dest = "/lp"; prune = False; verify = 0; excludes = []
    i = 2
    while i < len(args):
        if args[i] == "--exclude": excludes.append(args[i + 1].strip("/")); i += 2
        elif args[i] == "--dest": dest = args[i + 1]; i += 2
        elif args[i] == "--prune": prune = True; i += 1
        elif args[i] == "--verify": verify = int(args[i + 1]); i += 2
        else: i += 1
    ser = serial.Serial(port, 115200, timeout=10)
    time.sleep(0.5); ser.reset_input_buffer()
    def cmd(line, expect_prefix=None):
        ser.write((line + "\n").encode()); 
        while True:
            r = ser.readline().decode("utf-8", "replace").strip()
            if not r: raise RuntimeError("timeout waiting for reply to: " + line)
            if r.startswith("[") : continue
            if expect_prefix is None or r.startswith(expect_prefix) or r.startswith("ERR"): return r
    for _ in range(5):
        try:
            if cmd("PING", "PONG") == "PONG": break
        except RuntimeError: pass
    else: sys.exit("no PONG from the loader; is the card loader firmware flashed?")
    # ensure directories; PROTECTED folders (user data that lives only on the card) are neither copied nor pruned
    PROTECTED = ("presets", "sets", "state", "rec", "wav")   # + the recorder's sessions and rendered WAVs (SPEC §9.23)
    def protected(rel): return any(part in PROTECTED for part in rel.split("/") if part)
    def excluded(rel): return protected(rel) or any(rel == e or rel.startswith(e + "/") for e in excludes)
    dirs = sorted({dest} | {os.path.join(dest, os.path.relpath(dp, src)).replace("\\", "/") for dp, _, _ in os.walk(src) if os.path.relpath(dp, src) != "." and not excluded(os.path.relpath(dp, src).replace("\\", "/"))})
    for d in dirs:
        parts = d.strip("/").split("/")
        for k in range(1, len(parts) + 1):
            cmd("MKDIR /" + "/".join(parts[:k]), "OK")
    files = []
    for dp, _, fs in os.walk(src):
        for f in fs:
            if f.startswith("."): continue
            local = os.path.join(dp, f); rel = os.path.relpath(local, src).replace("\\", "/")
            if excluded(rel): continue
            files.append((local, dest + "/" + rel, os.path.getsize(local)))
    total = sum(s for _, _, s in files); done = 0; copied = []; t0 = time.time(); skipped = 0
    # one LS per card directory instead of a STAT per file (each is a FAT directory scan on the device)
    remote_sizes = {}
    for d in dirs:
        ser.write(("LS " + d + "\n").encode())
        while True:
            r = ser.readline().decode("utf-8", "replace").strip()
            if r == "END" or not r: break
            if r.startswith("F "):
                _, sz, name = r.split(" ", 2); remote_sizes[d + "/" + name] = int(sz)
    for n, (local, remote, size) in enumerate(files):
        if remote_sizes.get(remote) == size: skipped += 1; done += size; continue
        r = cmd(f"PUT {size} {remote}", "GO")
        if r != "GO": print("  PUT refused:", remote, r); continue
        with open(local, "rb") as fh:
            left = size
            while left > 0:
                buf = fh.read(min(4096, left)); ser.write(buf); left -= len(buf)
                a = ser.readline().decode().strip()
                if a != "A": raise RuntimeError(f"bad ack '{a}' for {remote}")
        r = ser.readline().decode().strip()
        if not r.startswith("OK"): raise RuntimeError(f"PUT failed for {remote}: {r}")
        copied.append((local, remote)); done += size
        if n % 50 == 0 or size > 500000:
            el = time.time() - t0; rate = (done / 1024) / el if el > 0 else 0
            print(f"  {n + 1}/{len(files)} {done / 1048576:.1f}/{total / 1048576:.1f} MB  {rate:.0f} KB/s  {remote}", flush=True)
    el = time.time() - t0
    print(f"done: {len(copied)} copied, {skipped} skipped (same size), {done / 1048576:.1f} MB in {el:.0f} s")
    if prune:
        def walk(remote_dir, local_dir):
            if os.path.basename(remote_dir) in PROTECTED: print("  (protected, not pruned):", remote_dir); return
            ser.write(("LS " + remote_dir + "\n").encode()); entries = []
            while True:
                r = ser.readline().decode("utf-8", "replace").strip()
                if r == "END" or not r: break
                entries.append(r)
            for e in entries:
                if e.startswith("D "):
                    name = e[2:]; sub = os.path.join(local_dir, name)
                    if os.path.isdir(sub): walk(remote_dir + "/" + name, sub)
                    else: print("  (dir not local, left in place):", remote_dir + "/" + name)
                elif e.startswith("F "):
                    _, _, name = e.split(" ", 2)
                    if not os.path.isfile(os.path.join(local_dir, name)): print("  prune", remote_dir + "/" + name, cmd("RM " + remote_dir + "/" + name, "OK"))
        walk(dest, src)
    if verify and copied:
        for local, remote in random.sample(copied, min(verify, len(copied))):
            with open(local, "rb") as fh: want = "%08x" % (zlib.crc32(fh.read()) & 0xFFFFFFFF)
            got = cmd("CRC " + remote, "C").split()[-1]
            print("  verify", remote, "OK" if got == want else f"MISMATCH {got} != {want}")

if __name__ == "__main__":
    main()
