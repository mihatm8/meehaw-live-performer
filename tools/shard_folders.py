#!/usr/bin/env python3
"""Shard every leaf content folder with more than MAX files into subfolders s00, s01, ... of at most MAX files
(sorted by name). FAT directory operations on the Cardputer are linear in the folder size (SPEC §9.2), so small
folders make both the firmware's opens and the USB card sync fast. The firmware reaches at most TREE_MAX
(kTreeMax) shards per folder, so a folder that would need more gets larger shards instead (ceil(files / TREE_MAX),
reported). Idempotent. Usage: shard_folders.py ROOT [--max 64]"""
import os, sys
root = sys.argv[1]; mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 64
TREE_MAX = 12          # kTreeMax: shards the firmware keeps per content folder
sharded = 0
for dp, dirs, files in os.walk(root):
    media = sorted(f for f in files if f.lower().endswith((".mid", ".wav")) and not f.startswith("."))
    if len(media) <= mx: continue
    size = mx
    if (len(media) + mx - 1) // mx > TREE_MAX:
        size = (len(media) + TREE_MAX - 1) // TREE_MAX
        print(f"  {os.path.relpath(dp, root)}: {len(media)} files need more than {TREE_MAX} shards of {mx}: using shards of {size}")
    for i in range(0, len(media), size):
        sub = os.path.join(dp, "s%02d" % (i // size)); os.makedirs(sub, exist_ok=True)
        for f in media[i:i + size]: os.replace(os.path.join(dp, f), os.path.join(sub, f))
    idx = os.path.join(dp, "_index.txt")
    if os.path.exists(idx): os.remove(idx)
    sharded += 1; print(f"  {os.path.relpath(dp, root)}: {len(media)} files -> {(len(media) + size - 1) // size} shards of <= {size}")
print(f"{sharded} folders sharded")
