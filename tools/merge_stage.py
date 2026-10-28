#!/usr/bin/env python3
"""Merge staging mirrors (sdcard/lp_stage/<key>/lp/...) into the card image (sdcard/lp).

Usage:
  python3 -I tools/merge_stage.py sdcard/lp sdcard/lp_stage/m1/lp [sdcard/lp_stage/m2/lp ...] [--dry-run] [--log FILE]

Every file of a stage is copied to the same relative path under the card's lp folder. A file that already exists on
the card is never overwritten: an identical copy (same bytes) is skipped silently, a different one is skipped and
reported as a collision. Stages hold flat key folders while the card may already be sharded (`sNN` subfolders of
shard_folders.py); a card folder that receives files and is a shard root is flattened first (its shard files are
moved back up, the empty shards and the stale `_index.txt` removed), so that one `shard_folders.py --max 64` run
afterwards re-shards the merged folder in name order and `write_indexes.py` rewrites every index. Folders that
receive nothing are left as they are. Nothing under `presets/`, `sets/` or `state/` is ever touched.

Stdlib only; run with -I. Prints a per-stage and per-folder summary and writes the same (with every collision and
flattened folder) to --log.
"""
import argparse
import filecmp
import os
import shutil
import sys
import time

MEDIA = (".mid", ".wav")
PROTECTED = ("presets", "sets", "state")


def is_shard(name):
    return len(name) == 3 and name[0] == "s" and name[1:].isdigit()


def is_media(name):
    return name.lower().endswith(MEDIA) and not name.startswith(".")


def existing(dst):
    """The card's copy of dst: flat, or inside a shard subfolder (what a dry run would flatten first)."""
    if os.path.exists(dst):
        return dst
    folder, name = os.path.split(dst)
    if os.path.isdir(folder):
        for d in os.listdir(folder):
            p = os.path.join(folder, d, name)
            if is_shard(d) and os.path.isfile(p):
                return p
    return None


def flatten(folder, log, dry):
    """Move every media file of folder/sNN up into folder; remove the shard folders and indexes. Returns
    (moved, problems)."""
    moved, problems = 0, []
    shards = sorted(d for d in os.listdir(folder) if is_shard(d) and os.path.isdir(os.path.join(folder, d)))
    for s in shards:
        sp = os.path.join(folder, s)
        for name in sorted(os.listdir(sp)):
            src = os.path.join(sp, name)
            if name == "_index.txt":
                if not dry:
                    os.remove(src)
                continue
            if not os.path.isfile(src):
                problems.append("unexpected entry in shard: %s" % src)
                continue
            dst = os.path.join(folder, name)
            if os.path.exists(dst):
                if filecmp.cmp(src, dst, shallow=False):
                    if not dry:
                        os.remove(src)
                    log.append("  duplicate inside tree removed: %s" % src)
                else:
                    problems.append("shard file differs from a flat file of the same name: %s" % src)
                continue
            if not dry:
                os.replace(src, dst)
            moved += 1
        if not dry:
            try:
                os.rmdir(sp)
            except OSError:
                problems.append("shard folder not empty after flattening: %s" % sp)
    idx = os.path.join(folder, "_index.txt")
    if os.path.isfile(idx) and not dry:
        os.remove(idx)
    return moved, problems


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("card", help="the card's lp folder (sdcard/lp)")
    ap.add_argument("stages", nargs="+", help="stage lp folders (sdcard/lp_stage/<key>/lp)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--log", help="write the full report here")
    args = ap.parse_args(argv)
    card = os.path.abspath(args.card)
    if not os.path.isdir(card):
        sys.exit("no such folder: %s" % card)
    log, dry = [], args.dry_run
    t0 = time.time()
    flattened = {}
    total_copied = total_bytes = total_identical = 0
    collisions = []
    per_folder = {}
    for stage in args.stages:
        stage = os.path.abspath(stage)
        if not os.path.isdir(stage):
            sys.exit("no such stage: %s" % stage)
        files = []
        for dp, dirs, fs in os.walk(stage):
            dirs.sort()
            for f in sorted(fs):
                if f.startswith("."):
                    continue
                rel = os.path.relpath(os.path.join(dp, f), stage).replace(os.sep, "/")
                if any(p in PROTECTED for p in rel.split("/")[:-1]):
                    log.append("  skipped (protected folder): %s" % rel)
                    continue
                if not is_media(f):
                    log.append("  skipped (not media): %s" % rel)
                    continue
                files.append(rel)
        copied = identical = coll = nbytes = 0
        for rel in files:
            src = os.path.join(stage, rel)
            dst = os.path.join(card, rel)
            folder = os.path.dirname(dst)
            if folder not in flattened:
                if os.path.isdir(folder):
                    shards = [d for d in os.listdir(folder) if is_shard(d) and os.path.isdir(os.path.join(folder, d))]
                    if shards:
                        moved, problems = flatten(folder, log, dry)
                        flattened[folder] = moved
                        log.append("flattened %s: %d files out of %d shards" % (os.path.relpath(folder, card), moved, len(shards)))
                        for p in problems:
                            log.append("  PROBLEM " + p)
                            collisions.append(p)
                    else:
                        flattened[folder] = 0
                else:
                    if not dry:
                        os.makedirs(folder, exist_ok=True)
                    flattened[folder] = 0
            have = existing(dst)
            if have:
                if filecmp.cmp(src, have, shallow=False):
                    identical += 1
                else:
                    coll += 1
                    collisions.append("%s (card %d B, stage %d B) from %s" % (rel, os.path.getsize(have), os.path.getsize(src), os.path.basename(os.path.dirname(stage))))
                continue
            if not dry:
                shutil.copy2(src, dst)
            copied += 1
            nbytes += os.path.getsize(src)
            per_folder[os.path.dirname(rel)] = per_folder.get(os.path.dirname(rel), 0) + 1
        line = "%s: %d files, %d copied (%.1f MB), %d identical already on the card, %d collisions" % (
            os.path.relpath(stage, os.getcwd()), len(files), copied, nbytes / 1048576, identical, coll)
        print(line)
        log.insert(0, line)
        total_copied += copied
        total_bytes += nbytes
        total_identical += identical
    print("flattened %d sharded folders before copying (%d files moved up)" % (
        sum(1 for v in flattened.values() if v), sum(flattened.values())))
    if collisions:
        print("%d collisions / problems (the card's file was kept, the stage's file was not copied):" % len(collisions))
        for c in collisions:
            print("  " + c)
    summary = "TOTAL: %d copied (%.1f MB), %d identical, %d collisions, %.1f s%s" % (
        total_copied, total_bytes / 1048576, total_identical, len(collisions), time.time() - t0, " (dry run)" if dry else "")
    print(summary)
    if args.log:
        with open(args.log, "w") as f:
            f.write(summary + "\n")
            f.write("\n".join(log) + "\n")
            f.write("\ncollisions:\n" + "\n".join("  " + c for c in collisions) + "\n")
            f.write("\nfiles added per folder:\n")
            for k in sorted(per_folder):
                f.write("  %-48s %d\n" % (k, per_folder[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
