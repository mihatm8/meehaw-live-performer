#!/usr/bin/env python3
"""Create or refresh `_index.txt` in every content folder of a Live Player SD card (SPEC §9.2).

Usage:
  python3 -I tools/write_indexes.py <sdcard root> [--check] [--prune] [--quiet] [--max-entries N]

Folders covered: everything under lp/midi, and under lp/packs/<Genre>/{drum/*, vocal/*, fx, inst/*, loops/*/*}
(recursively, so shard subfolders get their own index). A folder that holds .mid or .wav files gets an
`_index.txt` listing those basenames, one per line, LF terminated, sorted case-insensitively. Dotfiles,
`_index.txt` itself, other extensions and names the firmware cannot resolve (longer than MAX_NAME bytes)
are never listed. The firmware counts entries as the lines of this file and resolves ordinal n to line n in
4 KB reads, so the directory is never walked.

Firmware conventions the tool checks (warnings, never fatal):
  * a folder with an index is a leaf: subfolders next to media are never looked at, so a key folder is
    EITHER a leaf of media files OR a shard root holding only subfolders (each with its own index);
  * shard subfolder names must be shorter than TREE_NAME_LEN (kTreeNameLen) characters;
  * a content folder holds at most TREE_MAX (kTreeMax) shards: more are unreachable (an ERROR, exit 2; a folder
    at the limit gets a warning since its next shard would be lost — shard_folders.py grows the shard size);
  * a folder with more than --max-entries files should be sharded: on FAT every open by name (the index,
    every picked file) scans the directory linearly, which costs tens of ms in a 700-entry folder.

  --check   report what would change and exit 1 if any index is missing or stale; writes nothing
  --prune   delete an `_index.txt` in a covered folder that holds no media any more (default: warn)
  --quiet   only print the summary line and warnings (dropped names, unreachable subfolders); the
            sharding advice for large folders is left out

Stdlib only; run with -I so nothing is imported from the card or the working directory.
"""
import argparse
import os
import sys

INDEX = "_index.txt"
MEDIA = {".mid", ".wav"}
MAX_NAME = 95          # kMaxNameLen - 1: longer names cannot be resolved by the firmware
TREE_NAME_LEN = 8      # kTreeNameLen: a shard subfolder name must be shorter than this ("s00".."s11")
TREE_MAX = 12          # kTreeMax: shard subfolders the firmware keeps per content folder; the rest are unreachable
MAX_ENTRIES = 100      # default --max-entries: above this a folder should be sharded (FAT open cost)


def covered_roots(root):
    """Top folders whose whole subtree is indexed."""
    midi = os.path.join(root, "lp", "midi")
    if os.path.isdir(midi):
        yield midi
    packs = os.path.join(root, "lp", "packs")
    if not os.path.isdir(packs):
        return
    for genre in sorted(os.listdir(packs)):
        g = os.path.join(packs, genre)
        if genre.startswith(".") or not os.path.isdir(g):
            continue
        for sub in ("drum", "vocal", "inst"):
            d = os.path.join(g, sub)
            if not os.path.isdir(d):
                continue
            for leaf in sorted(os.listdir(d)):
                p = os.path.join(d, leaf)
                if not leaf.startswith(".") and os.path.isdir(p):
                    yield p
        # loops/<category>/<Key>: key folders are addressed by name like vocal/<Key> (docs/LOOPS.md), so the
        # category folder is not a shard root and each key folder is a covered leaf
        loops = os.path.join(g, "loops")
        if os.path.isdir(loops):
            for cat in sorted(os.listdir(loops)):
                c = os.path.join(loops, cat)
                if cat.startswith(".") or not os.path.isdir(c):
                    continue
                for key in sorted(os.listdir(c)):
                    p = os.path.join(c, key)
                    if not key.startswith(".") and os.path.isdir(p):
                        yield p
        fx = os.path.join(g, "fx")
        if os.path.isdir(fx):
            yield fx


def content_folders(root):
    """Every folder (depth-first, sorted) beneath the covered roots."""
    seen = set()
    for top in covered_roots(root):
        for dirpath, dirnames, _ in os.walk(top):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            if dirpath not in seen:
                seen.add(dirpath)
                yield dirpath


def scan_folder(folder, rel, warnings):
    """Media basenames (sorted as the firmware expects) and the subfolder names of one folder."""
    names, subdirs = [], []
    with os.scandir(folder) as it:
        for e in it:
            if e.name.startswith("."):
                continue
            if e.is_dir(follow_symlinks=False):
                subdirs.append(e.name)
                continue
            if e.name == INDEX or not e.is_file(follow_symlinks=False):
                continue
            if os.path.splitext(e.name)[1].lower() not in MEDIA:
                continue
            if "\n" in e.name or "\r" in e.name:
                warnings.append("dropped (newline in name): %s/%s" % (rel, e.name))
                continue
            if len(e.name.encode("utf-8")) > MAX_NAME:
                warnings.append("dropped (%d bytes, the firmware resolves at most %d): %s/%s" % (len(e.name.encode("utf-8")), MAX_NAME, rel, e.name))
                continue
            if e.name != e.name.strip():
                warnings.append("dropped (leading or trailing blanks): %s/%r" % (rel, e.name))
                continue
            names.append(e.name)
    names.sort(key=lambda n: (n.lower(), n))
    subdirs.sort()
    return names, subdirs


def is_shard_root(rel):
    """Folders whose subfolders the firmware discovers by walking: key folders and everything below them, and
    every covered pack folder. lp/midi and lp/midi/<Category> address their key folders by name."""
    parts = rel.replace(os.sep, "/").split("/")
    if parts[:2] == ["lp", "midi"]:
        return len(parts) >= 4
    return True


def walks_subfolders(rel):
    """Folders whose subfolder NAMES must fit the firmware's walk (kTreeNameLen): shard roots, and the
    lp/midi/<Category> folders whose key folders Beat and Percussion picks find by walking (SPEC §9.9)."""
    parts = rel.replace(os.sep, "/").split("/")
    return is_shard_root(rel) or (parts[:2] == ["lp", "midi"] and len(parts) == 3)


def check_layout(rel, names, subdirs, max_entries, warnings, advice, errors):
    """Firmware conventions: leaf vs shard root, shard name length and count, folder size (see module docstring)."""
    if names and subdirs:
        warnings.append("%s mixes %d media files with subfolders %s: an indexed folder is a leaf, the subfolders are unreachable"
                        % (rel, len(names), ", ".join(subdirs[:4]) + (" ..." if len(subdirs) > 4 else "")))
    for d in (subdirs if walks_subfolders(rel) else []):
        if len(d) >= TREE_NAME_LEN:
            warnings.append("%s/%s: subfolder name has %d characters, the firmware only sees names shorter than %d" % (rel, d, len(d), TREE_NAME_LEN))
    if is_shard_root(rel):
        if len(subdirs) > TREE_MAX:
            errors.append("%s holds %d subfolders, the firmware only reaches the first %d (kTreeMax): %d shard(s) of files are lost"
                          % (rel, len(subdirs), TREE_MAX, len(subdirs) - TREE_MAX))
        elif len(subdirs) == TREE_MAX:
            warnings.append("%s holds %d subfolders, the firmware's limit (kTreeMax): a further shard would be unreachable" % (rel, len(subdirs)))
    if len(names) > max_entries:
        advice.append("%s holds %d files: shard it into subfolders of <= %d (every open by name scans a FAT directory linearly)"
                      % (rel, len(names), max_entries))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("root", help="SD card root (the folder that contains lp/)")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--max-entries", type=int, default=MAX_ENTRIES, help="warn about folders with more files than this (default %d)" % MAX_ENTRIES)
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, "lp")):
        sys.exit("no lp/ folder under %s" % root)

    written = unchanged = stale = folders = files = 0
    warnings, advice, errors = [], [], []
    for folder in content_folders(root):
        rel = os.path.relpath(folder, root)
        names, subdirs = scan_folder(folder, rel, warnings)
        check_layout(rel, names, subdirs, args.max_entries, warnings, advice, errors)
        index = os.path.join(folder, INDEX)
        if not names:
            if os.path.isfile(index):
                stale += 1
                if args.prune and not args.check:
                    os.remove(index)
                    if not args.quiet:
                        print("removed  %s (no media left)" % rel)
                else:
                    print("STALE    %s/%s lists files but the folder holds no media%s" % (rel, INDEX, "" if args.check else " (use --prune)"))
            continue
        folders += 1
        files += len(names)
        content = ("\n".join(names) + "\n").encode("utf-8")
        current = None
        if os.path.isfile(index):
            with open(index, "rb") as f:
                current = f.read()
        if current == content:
            unchanged += 1
            continue
        written += 1
        if args.check:
            print("%-8s %s (%d entries)" % ("missing" if current is None else "outdated", rel, len(names)))
            continue
        tmp = index + ".tmp"
        with open(tmp, "wb") as f:
            f.write(content)
        os.replace(tmp, index)
        if not args.quiet:
            print("%-8s %s (%d entries)" % ("created" if current is None else "updated", rel, len(names)))

    for e in errors:
        print("ERROR:   " + e)
    for w in warnings:
        print("warning: " + w)
    if not args.quiet:
        for a in advice:
            print("advice:  " + a)
    verb = "need writing" if args.check else "written"
    print("%d content folders, %d files listed: %d %s, %d unchanged, %d stale, %d errors, %d warnings, %d folders above %d entries"
          % (folders, files, written, verb, unchanged, stale, len(errors), len(warnings), len(advice), args.max_entries))
    if errors:
        return 2
    return 1 if (args.check and (written or stale)) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
