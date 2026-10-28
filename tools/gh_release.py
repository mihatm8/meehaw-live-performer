#!/usr/bin/env python3
"""Create (or reuse) a DRAFT GitHub release v<ver> on mihatm8/meehaw-live-performer and upload the release assets.

Usage:
    python3 -I tools/gh_release.py --dry-run                      # what would happen: no network, no token
    python3 -I tools/gh_release.py                                # draft v<VERSION> from dist/release-<VERSION>/
    python3 -I tools/gh_release.py --version 0.9.1 --dir dist/release-0.9.1 --notes notes.md --replace

The release folder is what tools/make_release.sh produced: every regular file in it becomes an asset (the ROMs,
SHA256SUMS, the free card bundle meehaw-card-free*.zip and its manifest when present, LICENSE and CREDITS.md (the
notices the binaries must travel with), the sticker PDFs, MANUAL.md, FLASHING.md, CARD_IMPORT.md, ...). --exclude GLOB leaves files out, --asset PATH adds files from elsewhere.

Token: the github.com password git has stored (`git credential fill`, i.e. the osxkeychain helper on this Mac), or
GITHUB_TOKEN / GH_TOKEN from the environment. It is never printed. The token needs the `repo` scope (classic) or
"Contents: read and write" (fine-grained) on the repository.

Idempotent: an existing DRAFT with the same tag is reused (its title / notes are left alone unless --update-notes),
assets that are already there are skipped (--replace deletes and re-uploads them), a half-uploaded asset is removed and
sent again. A PUBLISHED release with that tag is never touched: bump the version. The result stays a draft — read it on
GitHub, then publish there (the tag v<ver> is created on `main` at that moment).

Stdlib only (urllib, json, subprocess). Run with python3 -I.
"""
import argparse
import fnmatch
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://api.github.com"
DEFAULT_REPO = "mihatm8/meehaw-live-performer"
USER_AGENT = "meehaw-gh-release/1.0 (python urllib)"
ROOT = Path(__file__).resolve().parent.parent

# Order of the assets on the release page (first match wins, the rest alphabetically).
ASSET_ORDER = ["meehaw-player-*-full.bin", "meehaw-player-*-app.bin", "meehaw-carddrive-*-app.bin",
               "meehaw-cardloader-*-app.bin", "SHA256SUMS", "meehaw-card-free*.zip", "*.manifest.txt",
               "FLASHING.md", "GETTING_STARTED.md", "CARD_IMPORT.md", "MANUAL.md", "KEYMAP.md", "LICENSE", "CREDITS.md",
               "sticker-*.pdf", "*.pdf"]


class ApiError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------- helpers
def read_version():
    try:
        return (ROOT / "VERSION").read_text().strip()
    except OSError:
        return ""


def fmt_size(n):
    return "%.1f MB" % (n / 1048576) if n >= 1048576 else "%.1f KB" % (n / 1024) if n >= 1024 else "%d B" % n


def token_from_git():
    """The stored github.com credential through git's helper. Returns (token, error)."""
    try:
        p = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                           capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "git credential fill did not run: %s" % e
    if p.returncode != 0:
        return None, "git credential fill exited %d%s" % (p.returncode, (": " + p.stderr.strip()) if p.stderr.strip() else "")
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            tok = line[len("password="):].strip()
            if tok:
                return tok, None
    return None, "git credential fill returned no password for https://github.com"


def get_token():
    for var in ("GITHUB_TOKEN", "GH_TOKEN"):
        if os.environ.get(var):
            return os.environ[var], var
    tok, err = token_from_git()
    if tok:
        return tok, "git credential fill"
    sys.exit("no token: %s (set GITHUB_TOKEN, or store a github.com token for git: `git credential approve`)" % err)


def api(token, method, url, body=None, timeout=60):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("User-Agent", USER_AGENT)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return json.loads(raw) if raw.strip() else None
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:600]
        raise ApiError("%s %s -> HTTP %d: %s" % (method, url, e.code, detail))
    except urllib.error.URLError as e:
        raise ApiError("%s %s -> %s" % (method, url, e.reason))


def api_pages(token, url):
    """Every item of a paginated list endpoint."""
    page, out = 1, []
    while True:
        sep = "&" if "?" in url else "?"
        items = api(token, "GET", "%s%sper_page=100&page=%d" % (url, sep, page)) or []
        out.extend(items)
        if len(items) < 100:
            return out
        page += 1


class Progress:
    """A file wrapper http.client reads in blocks; prints the upload progress to stderr."""

    def __init__(self, f, total, label):
        self.f, self.total, self.label = f, total, label
        self.done, self.t0, self.last = 0, time.monotonic(), 0.0
        self.show(force=True)

    def read(self, n=-1):
        chunk = self.f.read(n if n is not None and n > 0 else 1 << 20)
        self.done += len(chunk)
        self.show(force=not chunk)
        return chunk

    def show(self, force=False):
        now = time.monotonic()
        if not force and now - self.last < 0.2:
            return
        self.last = now
        pct = 100.0 * self.done / self.total if self.total else 100.0
        rate = self.done / max(now - self.t0, 1e-6)
        bar = "#" * int(pct / 5) + "." * (20 - int(pct / 5))
        sys.stderr.write("\r  %-40s %9s [%s] %5.1f%%  %s/s   " % (self.label[:40], fmt_size(self.total), bar, pct, fmt_size(rate)))
        sys.stderr.flush()
        if self.done >= self.total and force:
            sys.stderr.write("\n")


def upload_asset(token, upload_url, path):
    base = upload_url.split("{", 1)[0]
    url = base + "?" + urllib.parse.urlencode({"name": path.name})
    size = path.stat().st_size
    with open(path, "rb") as f:
        body = Progress(f, size, path.name)
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Authorization", "Bearer " + token)
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", USER_AGENT)
        req.add_header("Content-Type", "application/octet-stream")
        req.add_header("Content-Length", str(size))      # explicit: urllib would otherwise send the stream chunked, which the upload endpoint refuses
        try:
            with urllib.request.urlopen(req, timeout=900) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            sys.stderr.write("\n")
            raise ApiError("upload %s -> HTTP %d: %s" % (path.name, e.code, e.read().decode("utf-8", "replace")[:600]))
        except (urllib.error.URLError, OSError) as e:
            sys.stderr.write("\n")
            raise ApiError("upload %s -> %s" % (path.name, e))


def collect_assets(release_dir, extra, excludes):
    files = [p for p in sorted(release_dir.iterdir()) if p.is_file() and not p.name.startswith(".")]
    files += [Path(e).resolve() for e in extra]
    files = [p for p in files if not any(fnmatch.fnmatch(p.name, g) for g in excludes)]
    for p in files:
        if not p.is_file():
            sys.exit("not a file: %s" % p)

    def rank(p):
        for i, pat in enumerate(ASSET_ORDER):
            if fnmatch.fnmatch(p.name, pat):
                return i, p.name
        return len(ASSET_ORDER), p.name
    files.sort(key=rank)
    seen = set()
    for p in files:
        if p.name in seen:
            sys.exit("two assets would have the same name: %s" % p.name)
        seen.add(p.name)
    return files


def default_notes(ver, release_dir, assets):
    full = [p.name for p in assets if fnmatch.fnmatch(p.name, "meehaw-player-*-full.bin")]
    bundle = [p.name for p in assets if fnmatch.fnmatch(p.name, "meehaw-card-free*.zip")]
    sums = ""
    try:
        sums = (release_dir / "SHA256SUMS").read_text().strip()
    except OSError:
        pass
    names = {p.name for p in assets}

    def asset_or_repo(name, repo_path):
        """`name`, an asset of this release — or, when the folder lacks it, where it is in the repository."""
        return "`%s`, an asset of this release" % name if name in names else "`%s` in the repository" % repo_path
    lines = [
        "meehaw live performer v%s — firmware for the M5Stack Cardputer ADV (ESP32-S3, 8 MB flash)." % ver,
        "",
        "**Flash it**: `%s` goes to address 0x0 and holds the player (ota_0) and the USB drive firmware (ota_1) with the "
        "bootloader, partition table and otadata — the one file a new device needs. FLASHING.md has the esptool commands "
        "for macOS / Linux / Windows, the web flasher, the recovery steps and the update images:" % (full[0] if full else "meehaw-player-%s-full.bin" % ver),
        "",
        "| file | address | what |",
        "|---|---|---|",
        "| `meehaw-player-%s-full.bin` | 0x0 | everything (first flash) |" % ver,
        "| `meehaw-player-%s-app.bin` | 0x10000 | the player alone (update) |" % ver,
        "| `meehaw-carddrive-%s-app.bin` | 0x340000 | the USB drive firmware alone |" % ver,
        "| `meehaw-cardloader-%s-app.bin` | 0x10000 | the serial card loader; it replaces the player until the player app is flashed again |" % ver,
        "",
        "**The card**: the player reads everything from `/lp` on the microSD card (GETTING_STARTED.md, the manual MANUAL.md, "
        "every key in KEYMAP.md, printable key stickers in the PDFs).",
    ]
    if bundle:
        lines += ["", "**Card content**: `%s` holds the shareable part of the library — this project's own generated clips (GEN) and the "
                  "chord progressions of ldrolez/free-midi-chords (MIT). The owner's commercial-pack derivatives are not "
                  "in it and never will be; build your own from your packs with the tools in the repository. The card layout and the three "
                  "ways to fill a card: %s." % (bundle[0], asset_or_repo("CARD_IMPORT.md", "docs/CARD_IMPORT.md"))]
    else:
        lines += ["", "**Card content**: no card bundle in this release; build the card from your own library with the tools in the "
                  "repository. The card layout and the three ways to fill a card: %s." % asset_or_repo("CARD_IMPORT.md", "docs/CARD_IMPORT.md")]
    lines += ["", "Verify a download with `shasum -a 256 -c SHA256SUMS` (Linux: `sha256sum -c SHA256SUMS`).", ""]
    if sums:
        lines += ["<details><summary>SHA256SUMS</summary>", "", "```", sums, "```", "", "</details>", ""]
    lines += ["Miha Ambroz (@mihatm8). **Licence**: MIT, %s — it covers the firmware, the tools and the docs. The third-party "
              "code the binaries link (M5Unified, M5GFX, TinyUSB: MIT; arduino-esp32: LGPL-2.1; ESP-IDF: Apache-2.0; FatFs) and the "
              "content sources (ldrolez/free-midi-chords: MIT), with their notices: %s. Keep both files with any copy of the binaries; "
              "the card bundle's own licences are in `lp/LICENSES.md` inside it."
              % (asset_or_repo("LICENSE", "LICENSE"), asset_or_repo("CREDITS.md", "docs/CREDITS.md"))]
    return "\n".join(lines)


# ------------------------------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="draft GitHub release + asset upload for the meehaw live performer")
    ap.add_argument("--version", "-v", default=None, help="release version (default: the VERSION file; tag = v<version>)")
    ap.add_argument("--dir", default=None, help="release folder (default dist/release-<version>)")
    ap.add_argument("--repo", default=DEFAULT_REPO, help="owner/name (default %s)" % DEFAULT_REPO)
    ap.add_argument("--target", default="main", help="branch or commit the tag is created on when published (default main)")
    ap.add_argument("--title", default=None, help="release title (default 'meehaw live performer v<version>')")
    ap.add_argument("--notes", default=None, help="markdown file with the release notes (default: generated)")
    ap.add_argument("--update-notes", action="store_true", help="write the title / notes into an existing draft too")
    ap.add_argument("--prerelease", choices=["auto", "yes", "no"], default="auto", help="auto = yes while the major version is 0")
    ap.add_argument("--asset", action="append", default=[], help="extra file to upload (repeatable)")
    ap.add_argument("--exclude", action="append", default=[], help="glob of release-folder files to leave out (repeatable)")
    ap.add_argument("--replace", action="store_true", help="delete and re-upload assets that already exist on the draft")
    ap.add_argument("--dry-run", action="store_true", help="print what would be done; no network, no token")
    args = ap.parse_args()

    ver = (args.version or read_version()).strip().lstrip("v")
    if not ver:
        sys.exit("no version: pass --version or write the VERSION file")
    tag = "v" + ver
    release_dir = Path(args.dir) if args.dir else ROOT / "dist" / ("release-%s" % ver)
    if not release_dir.is_dir():
        sys.exit("no release folder %s (run: sh tools/make_release.sh %s)" % (release_dir, ver))
    assets = collect_assets(release_dir, args.asset, args.exclude)
    if not assets:
        sys.exit("nothing to upload in %s" % release_dir)
    title = args.title or "meehaw live performer v%s" % ver
    notes = Path(args.notes).read_text() if args.notes else default_notes(ver, release_dir, assets)
    prerelease = (ver.split(".")[0] == "0") if args.prerelease == "auto" else args.prerelease == "yes"
    total = sum(p.stat().st_size for p in assets)

    print("release %s on %s  (draft; target %s; prerelease %s)" % (tag, args.repo, args.target, "yes" if prerelease else "no"))
    print("assets from %s (%d files, %s):" % (release_dir, len(assets), fmt_size(total)))
    for p in assets:
        print("  %-44s %10d bytes" % (p.name, p.stat().st_size))

    if args.dry_run:
        print("\n--dry-run: these calls would be made (no network, the token was not read):")
        print("  1. token: GITHUB_TOKEN / GH_TOKEN, else `git credential fill` for https://github.com (never printed)")
        print("  2. GET  %s/repos/%s/releases?per_page=100     -> look for a DRAFT with tag_name %s" % (API, args.repo, tag))
        print("  3. POST %s/repos/%s/releases  {tag_name: %s, target_commitish: %s, name: %r, draft: true, prerelease: %s}"
              % (API, args.repo, tag, args.target, title, "true" if prerelease else "false"))
        print("     (skipped when the draft exists%s; a PUBLISHED %s aborts the run)" % (", notes rewritten with --update-notes" if args.update_notes else "", tag))
        print("  4. GET  the draft's assets; for each file above: skip when present%s, else" % (" (or delete + re-upload with --replace)" if args.replace else ""))
        print("     POST https://uploads.github.com/repos/%s/releases/<id>/assets?name=<file>  (application/octet-stream, with progress)" % args.repo)
        print("  5. print the draft's URL (https://github.com/%s/releases/tag/%s once published; the draft shows under /releases)" % (args.repo, tag))
        print("\nrelease notes (%d chars)%s:\n" % (len(notes), " from " + args.notes if args.notes else ", generated"))
        print("\n".join("    " + l for l in notes.splitlines()[:16]))
        if len(notes.splitlines()) > 16:
            print("    ... (%d more lines)" % (len(notes.splitlines()) - 16))
        return 0

    token, source = get_token()
    print("token: from %s" % source)
    repo_url = "%s/repos/%s" % (API, args.repo)
    try:
        who = api(token, "GET", API + "/user")
        print("authenticated as %s" % who.get("login", "?"))
        existing = [r for r in api_pages(token, repo_url + "/releases") if r.get("tag_name") == tag]
        release = None
        if existing:
            release = existing[0]
            if not release.get("draft"):
                sys.exit("%s is already PUBLISHED on %s (%s): bump the version instead of re-releasing it" % (tag, args.repo, release.get("html_url")))
            print("reusing the existing draft %s (id %s)" % (tag, release["id"]))
            if args.update_notes:
                release = api(token, "PATCH", "%s/releases/%s" % (repo_url, release["id"]), {"name": title, "body": notes})
                print("title and notes updated")
        else:
            release = api(token, "POST", repo_url + "/releases",
                          {"tag_name": tag, "target_commitish": args.target, "name": title, "body": notes, "draft": True, "prerelease": prerelease})
            print("created the draft %s (id %s)" % (tag, release["id"]))

        have = {a["name"]: a for a in api_pages(token, "%s/releases/%s/assets" % (repo_url, release["id"]))}
        for p in assets:
            a = have.get(p.name)
            if a is not None:
                if a.get("state") != "uploaded":
                    print("  %s: a broken upload (%s) is there: removing it" % (p.name, a.get("state")))
                    api(token, "DELETE", "%s/releases/assets/%s" % (repo_url, a["id"]))
                elif args.replace or a.get("size") != p.stat().st_size:
                    print("  %s: %s (%s on GitHub, %s here): replacing" % (p.name, "--replace" if args.replace else "size differs", fmt_size(a.get("size", 0)), fmt_size(p.stat().st_size)))
                    api(token, "DELETE", "%s/releases/assets/%s" % (repo_url, a["id"]))
                else:
                    print("  %-44s already uploaded (%s), skipped" % (p.name, fmt_size(a.get("size", 0))))
                    continue
            for attempt in range(1, 4):
                try:
                    res = upload_asset(token, release["upload_url"], p)
                    if res.get("state") != "uploaded":
                        raise ApiError("%s ended in state %s" % (p.name, res.get("state")))
                    break
                except ApiError as e:
                    print("  attempt %d failed: %s" % (attempt, e))
                    if attempt == 3:
                        raise
                    # a failed upload can leave a 'starter' asset of that name behind: remove it before the retry
                    for a in api_pages(token, "%s/releases/%s/assets" % (repo_url, release["id"])):
                        if a["name"] == p.name:
                            api(token, "DELETE", "%s/releases/assets/%s" % (repo_url, a["id"]))
                    time.sleep(3 * attempt)
        release = api(token, "GET", "%s/releases/%s" % (repo_url, release["id"]))
        names = sorted(a["name"] for a in release.get("assets", []))
        print("\ndraft %s holds %d asset(s): %s" % (tag, len(names), ", ".join(names)))
        print("draft URL (edit / publish there): %s" % release.get("html_url"))
        return 0
    except ApiError as e:
        sys.exit("GitHub API error: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
