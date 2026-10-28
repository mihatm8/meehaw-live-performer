#!/usr/bin/env python3
"""Send bring-up commands to the device and print its output.
Usage: serial_cmd.py PORT "cmd" "sleep 1.5" "cmd" ...   (a trailing 'sleep' keeps listening)"""
import sys, time, serial
port = sys.argv[1]; items = sys.argv[2:]
def pump(ser, secs):
    t0 = time.time(); buf = b""
    while time.time() - t0 < secs:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1); print("  <", line.decode("utf-8", "replace").rstrip(), flush=True)
with serial.Serial(port, 115200, timeout=0.1) as ser:
    pump(ser, 0.3)
    for it in items:
        if it.startswith("sleep "): pump(ser, float(it.split()[1])); continue
        print("> " + it, flush=True); ser.write((it + "\n").encode()); pump(ser, 0.4)
    pump(ser, 1.0)
