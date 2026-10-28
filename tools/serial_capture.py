#!/usr/bin/env python3
"""Reset the Cardputer (RTS pulse over USB-CDC) and capture serial output for N seconds.
Usage: serial_capture.py PORT SECONDS [--no-reset]"""
import sys, time, serial
port, secs = sys.argv[1], float(sys.argv[2])
reset = "--no-reset" not in sys.argv
with serial.Serial(port, 115200, timeout=0.2) as ser:
    if reset:
        ser.dtr = False; ser.rts = True; time.sleep(0.1); ser.rts = False
    t0 = time.time(); buf = b""
    while time.time() - t0 < secs:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode("utf-8", "replace").rstrip(), flush=True)
