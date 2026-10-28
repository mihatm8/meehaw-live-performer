#!/usr/bin/env python3
"""Pull card files to the computer over the USB serial port — for a computer whose policy blocks USB drives (SPEC §9.22).
Usage: card_pull.py PORT [REMOTE_DIR] [LOCAL_DIR]      defaults: /lp/packs/House/presets  ./pulled
       card_pull.py --selftest                          (no device: exercises both protocol dialects in-process)
Works with whichever firmware is running: the PLAYER (commands `ls DIR` -> "  F|D 0 <name>" ... "[ls] N entries",
`get FILE` -> "G <size>" + <size> raw bytes, "G -1" when missing) or the CARD LOADER (`LS DIR` -> "F <size> <name>" /
"D <name>" ... "END", `GET FILE` -> same "G" reply). The player keeps printing its [diag] lines between commands; they
are skipped while waiting for a reply, and cannot interleave with the bytes (the device sends a file in one go).
Walks REMOTE_DIR recursively, writes every file to LOCAL_DIR/<path relative to REMOTE_DIR>, and verifies the size:
the byte count received must equal the announced size (and, with the loader, the size the listing reported). With the
loader a local file whose size already matches is skipped (the player's listing has no sizes: everything is pulled).
Exit status 1 when any file failed or came back short."""
import os, sys, time

DEFAULT_REMOTE = "/lp/packs/House/presets"
DEFAULT_LOCAL = "./pulled"
STALL_S = 5.0          # no byte for this long inside a transfer = failure
REPLY_S = 8.0          # waiting for the "G" / listing lines (a long listing on the player may take a while)


class Device:
    """One firmware dialect over a serial-like object with read(n) -> bytes, write(bytes), timeout attribute."""
    def __init__(self, ser):
        self.ser = ser; self.buf = b""; self.loader = None

    # ---- line / byte plumbing -------------------------------------------------------------------------------------
    def _fill(self):
        chunk = self.ser.read(4096)
        if chunk: self.buf += chunk
        return bool(chunk)

    def readline(self, timeout):
        t0 = time.time()
        while b"\n" not in self.buf:
            if time.time() - t0 > timeout: return None
            if not self._fill(): time.sleep(0.005)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def readexact(self, n):
        out = bytearray(); last = time.time()
        while len(out) < n:
            if self.buf:
                take = min(n - len(out), len(self.buf)); out += self.buf[:take]; self.buf = self.buf[take:]; last = time.time(); continue
            if not self._fill():
                if time.time() - last > STALL_S: break
                time.sleep(0.002)
        return bytes(out)

    def send(self, line):
        self.ser.write((line + "\n").encode())

    # ---- detection ------------------------------------------------------------------------------------------------
    def detect(self):
        """True = card loader (answers PONG), False = player (ignores PING, answers `ls`)."""
        self.buf = b""; self.ser.read(65536)                 # drop whatever the device printed so far
        self.send("PING")
        t0 = time.time()
        while time.time() - t0 < 1.0:
            l = self.readline(1.0)
            if l is None: break
            if l.strip() == "PONG": self.loader = True; return True
        self.loader = False
        return False

    # ---- listing: [(name, isDir, size or None)] ---------------------------------------------------------------------
    def listdir(self, path):
        entries = []
        if self.loader:
            self.send("LS " + path)
            while True:
                l = self.readline(REPLY_S)
                if l is None: raise RuntimeError("timeout listing " + path)
                l = l.strip()
                if l == "END": break
                if l.startswith("F "): _, sz, name = l.split(" ", 2); entries.append((name, False, int(sz)))
                elif l.startswith("D "): entries.append((l[2:], True, None))
        else:
            self.send("ls " + path)
            while True:
                l = self.readline(REPLY_S)
                if l is None: raise RuntimeError("timeout listing " + path)
                s = l.strip()
                if s.startswith("[ls] "): break
                if not s or s.startswith("["): continue        # [diag] and other chatter
                parts = s.split(" ", 2)
                if len(parts) == 3 and parts[0] in ("F", "D"): entries.append((parts[2], parts[0] == "D", None))
        return entries

    def walk(self, root):
        """Yield (remote path, relative path, listed size or None) for every file below root."""
        stack = [(root, "")]
        while stack:
            path, rel = stack.pop(0)
            for name, isdir, size in self.listdir(path):
                full = path.rstrip("/") + "/" + name; r = (rel + "/" + name) if rel else name
                if isdir: stack.append((full, r))
                else: yield full, r, size

    # ---- one file -------------------------------------------------------------------------------------------------
    def get(self, path):
        """(announced size or -1, bytes). Lines before the 'G' header (the player's [diag]) are skipped."""
        self.send(("GET " if self.loader else "get ") + path)
        t0 = time.time()
        while True:
            l = self.readline(REPLY_S)
            if l is None or time.time() - t0 > REPLY_S: raise RuntimeError("no G reply for " + path)
            s = l.strip()
            if s.startswith("G "):
                try: size = int(s[2:])
                except ValueError: continue
                break
        if size < 0: return -1, b""
        return size, self.readexact(size)


def pull(dev, remote, local):
    """Returns (ok count, failed list)."""
    ok = 0; failed = []
    for full, rel, listed in dev.walk(remote):
        dst = os.path.join(local, rel.replace("/", os.sep))
        if listed is not None and os.path.isfile(dst) and os.path.getsize(dst) == listed:
            print(f"  = {rel} ({listed} B, same size)"); ok += 1; continue
        size, data = dev.get(full)
        if size < 0: print(f"  ! {rel}: missing on the card"); failed.append(rel); continue
        if len(data) != size or (listed is not None and listed != size):
            print(f"  ! {rel}: announced {size} B, listed {listed}, received {len(data)} B"); failed.append(rel); continue
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        with open(dst, "wb") as fh: fh.write(data)
        print(f"  + {rel} ({size} B)"); ok += 1
    return ok, failed


# ---- self-test: both dialects over an in-process fake port -------------------------------------------------------
class FakePort:
    """Emulates the player's or the loader's serial side over a dict {remote path: bytes}; dirs are implied.
    The player variant sprinkles [diag] lines before every reply (never inside a transfer)."""
    def __init__(self, files, loader):
        self.files = files; self.loader = loader; self.out = b""; self.inb = b""; self.timeout = 0.1

    def _dirs(self):
        d = set()
        for p in self.files:
            parts = p.strip("/").split("/")
            for k in range(1, len(parts)): d.add("/" + "/".join(parts[:k]))
        return d

    def _entries(self, path):
        path = path.rstrip("/"); out = []
        for p in sorted(self.files):
            if p.startswith(path + "/") and "/" not in p[len(path) + 1:]: out.append((p[len(path) + 1:], False, len(self.files[p])))
        for d in sorted(self._dirs()):
            if d.startswith(path + "/") and "/" not in d[len(path) + 1:]: out.append((d[len(path) + 1:], True, None))
        return out

    def write(self, data):
        self.inb += data
        while b"\n" in self.inb:
            line, self.inb = self.inb.split(b"\n", 1); self._handle(line.decode())

    def _handle(self, line):
        if self.loader:
            if line == "PING": self.out += b"PONG\r\n"
            elif line.startswith("LS "):
                for n, d, s in self._entries(line[3:]): self.out += (f"D {n}\r\n" if d else f"F {s} {n}\r\n").encode()
                self.out += b"END\r\n"
            elif line.startswith("GET "):
                data = self.files.get(line[4:])
                self.out += b"G -1\n" if data is None else f"G {len(data)}\n".encode() + data
            else: self.out += b"ERR unknown\r\n"
        else:
            self.out += b"[diag] t=3s render_max_us=1200 heap=13400\r\n"      # chatter before every reply
            if line.startswith("ls "):
                for n, d, s in self._entries(line[3:]): self.out += f"  {'D' if d else 'F'} 0 {n}\r\n".encode()
                self.out += b"[ls] 3 entries\r\n"
            elif line.startswith("get "):
                data = self.files.get(line[4:])
                self.out += b"G -1\n" if data is None else f"G {len(data)}\n".encode() + data
            # the player ignores unknown lines (PING) silently

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk


def selftest():
    import tempfile, shutil
    files = {
        "/lp/packs/House/presets/House_001_Dm.lpset": b"lp.version=2\r\nlp.label=first\n" + bytes(range(256)) * 20,   # binary, \r\n inside, > 4 KB
        "/lp/packs/House/presets/House_002_Am.lpset": b"",                                                               # empty file
        "/lp/packs/House/presets/sub/deep.txt": b"G 5\n[ls] 9 entries\nnot a header\n",                                  # content that looks like protocol lines
    }
    fails = 0
    for loader in (True, False):
        tmp = tempfile.mkdtemp()
        try:
            dev = Device(FakePort(files, loader))
            if dev.detect() != loader: print(f"FAIL detect loader={loader}"); fails += 1
            ok, failed = pull(dev, "/lp/packs/House/presets", tmp)
            if ok != 3 or failed: print(f"FAIL pull loader={loader}: ok={ok} failed={failed}"); fails += 1
            for p, data in files.items():
                rel = p[len("/lp/packs/House/presets/"):]
                with open(os.path.join(tmp, rel), "rb") as fh:
                    if fh.read() != data: print(f"FAIL content loader={loader}: {rel}"); fails += 1
            size, data = dev.get("/lp/packs/House/presets/nope.lpset")
            if size != -1: print(f"FAIL missing loader={loader}: {size}"); fails += 1
            if loader:                                            # same-size skip (listing sizes)
                ok2, failed2 = pull(dev, "/lp/packs/House/presets", tmp)
                if ok2 != 3 or failed2: print("FAIL rerun skip"); fails += 1
        finally:
            shutil.rmtree(tmp)
    # a short transfer (the device gives up) is reported, not written
    short = FakePort({"/lp/x/a.bin": b"abcdef"}, True)
    short._handle_orig = short._handle
    def cut(line):
        short._handle_orig(line)
        if line.startswith("GET "): short.out = short.out[:-2]  # drop the last 2 bytes
    short._handle = cut
    dev = Device(short); dev.loader = True
    tmp = tempfile.mkdtemp()
    try:
        global STALL_S; saved = STALL_S; STALL_S = 0.05
        ok, failed = pull(dev, "/lp/x", tmp); STALL_S = saved
        if ok != 0 or failed != ["a.bin"] or os.path.exists(os.path.join(tmp, "a.bin")): print(f"FAIL short transfer: ok={ok} failed={failed}"); fails += 1
    finally:
        shutil.rmtree(tmp)
    print("card_pull selftest:", "PASS" if fails == 0 else f"FAIL ({fails})")
    return fails == 0


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest": sys.exit(0 if selftest() else 1)
    if not args: print(__doc__); sys.exit(2)
    port = args[0]; remote = args[1] if len(args) > 1 else DEFAULT_REMOTE; local = args[2] if len(args) > 2 else DEFAULT_LOCAL
    import serial                                              # pyserial (tools/.venv)
    ser = serial.Serial(port, 115200, timeout=0.1)             # no DTR/RTS pulse: the device keeps running
    time.sleep(0.3)
    dev = Device(ser)
    loader = dev.detect()
    print(f"device: {'card loader' if loader else 'player'}; pulling {remote} -> {local}")
    t0 = time.time()
    ok, failed = pull(dev, remote, local)
    print(f"done: {ok} files ok, {len(failed)} failed in {time.time() - t0:.1f} s")
    for f in failed: print("  failed:", f)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
