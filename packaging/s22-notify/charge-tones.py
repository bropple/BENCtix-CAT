#!/usr/bin/env python3
"""The charger's sounds (D073), made at build time: a doodle-doo, three soft
mallet notes (short, short, long), rising for "plugged in" and falling for
"unplugged". 16-bit mono WAVs, for s22-play.

    charge-tones.py DIR      writes DIR/charging-started.wav and
                             DIR/charging-stopped.wav
"""
import math, os, struct, sys, wave

RATE = 48000
PEAK = 0.7                      # of full scale; s22-play sets the volume
NOTES = (523.25, 659.25, 783.99)         # C5 E5 G5
# (onset, length) in seconds: doo-dle-doooo
TIMING = ((0.0, 0.11), (0.11, 0.11), (0.22, 0.42))

def note(freq, length):
    """A mallet: a 4 ms attack, an exponential decay, a little octave"""
    out = []
    for i in range(int(length * RATE)):
        t = i / RATE
        env = min(1.0, t / 0.004) * math.exp(-t / (length / 3.5))
        out.append(env * (math.sin(2 * math.pi * freq * t)
                          + 0.25 * math.sin(4 * math.pi * freq * t)))
    return out

def tune(freqs):
    total = max(at + n for at, n in TIMING)
    buf = [0.0] * (int(total * RATE) + 1)
    for f, (at, n) in zip(freqs, TIMING):
        start = int(at * RATE)
        for i, s in enumerate(note(f, n)):
            buf[start + i] += s
    top = max(abs(s) for s in buf)
    return b"".join(struct.pack("<h", int(s / top * PEAK * 32767)) for s in buf)

def write(path, data):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    write(os.path.join(sys.argv[1], "charging-started.wav"), tune(NOTES))
    write(os.path.join(sys.argv[1], "charging-stopped.wav"), tune(NOTES[::-1]))
