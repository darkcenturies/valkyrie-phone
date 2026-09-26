"""Synthesise the phone's ringtones and text tones.

Every tone is made here from sine waves, plucked strings and noise, so the
phone ships no one else's recordings. The files go to assets/tones/ringtones
and assets/tones/texttones as 22 kHz mono .wav files; the build installs them
to valkyrie-phone-tones in the game folder, where the phone lists every .wav
it finds, named after the file.

    python generate-phone-tones.py
"""
import os
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "tones")
RATE = 22050
rng = np.random.default_rng(2007)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12.0)


def t_axis(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def env(n, attack=0.004, decay=0.5):
    t = np.arange(n) / RATE
    a = np.minimum(1.0, t / attack) if attack > 0 else 1.0
    return a * np.exp(-t / decay)


# --- Voices: each returns a mono float array --------------------------------

def marimba(midi, length=0.6):
    f = hz(midi)
    t = t_axis(length)
    n = len(t)
    s = np.sin(2 * np.pi * f * t) * env(n, 0.002, 0.22)
    s += 0.35 * np.sin(2 * np.pi * f * 3.9 * t) * env(n, 0.001, 0.05)
    s += 0.12 * np.sin(2 * np.pi * f * 9.2 * t) * env(n, 0.001, 0.02)
    return s


def xylophone(midi, length=0.5):
    f = hz(midi)
    t = t_axis(length)
    n = len(t)
    s = np.sin(2 * np.pi * f * t) * env(n, 0.001, 0.12)
    s += 0.5 * np.sin(2 * np.pi * f * 3.0 * t) * env(n, 0.001, 0.06)
    s += 0.2 * np.sin(2 * np.pi * f * 6.1 * t) * env(n, 0.001, 0.03)
    return s


def bell(midi, length=1.6, decay=0.7):
    f = hz(midi)
    t = t_axis(length)
    n = len(t)
    s = np.zeros(n)
    for ratio, amp, d in ((1.0, 1.0, 1.0), (2.0, 0.5, 0.7), (2.76, 0.4, 0.5), (5.4, 0.25, 0.25),
                          (8.93, 0.12, 0.12)):
        s += amp * np.sin(2 * np.pi * f * ratio * t) * env(n, 0.001, decay * d)
    return s / 2.2


def glass(midi, length=1.2):
    f = hz(midi)
    t = t_axis(length)
    n = len(t)
    s = np.sin(2 * np.pi * f * t) * env(n, 0.003, 0.45)
    s += 0.3 * np.sin(2 * np.pi * f * 2.32 * t) * env(n, 0.002, 0.2)
    s += 0.15 * np.sin(2 * np.pi * f * 4.25 * t) * env(n, 0.001, 0.1)
    return s


def pluck(midi, length=1.2, bright=0.5, decay=0.996):
    """Karplus-Strong: a burst of noise round a delay line."""
    f = hz(midi)
    period = max(2, int(RATE / f))
    n = int(length * RATE)
    buf = rng.uniform(-1, 1, period)
    # Darker strings start from smoothed noise.
    for _ in range(int((1 - bright) * 4)):
        buf = 0.5 * (buf + np.roll(buf, 1))
    out = np.zeros(n)
    for i in range(n):
        j = i % period
        out[i] = buf[j]
        buf[j] = decay * 0.5 * (buf[j] + buf[(j + 1) % period])
    return out * env(n, 0.001, length)


def square(midi, length=0.2, duty=0.5, decay=10.0):
    f = hz(midi)
    t = t_axis(length)
    n = len(t)
    phase = (f * t) % 1.0
    s = np.where(phase < duty, 1.0, -1.0)
    # A soft low-pass so it is not harsh through a phone speaker.
    k = np.ones(4) / 4
    s = np.convolve(s, k, mode="same")
    return 0.45 * s * env(n, 0.002, decay)


def sine(f, length, attack=0.005, decay=10.0):
    t = t_axis(length)
    return np.sin(2 * np.pi * f * t) * env(len(t), attack, decay)


def kick(length=0.35):
    t = t_axis(length)
    f = 45 + 110 * np.exp(-t / 0.04)
    phase = 2 * np.pi * np.cumsum(f) / RATE
    return np.sin(phase) * env(len(t), 0.001, 0.12)


def snare(length=0.25):
    t = t_axis(length)
    n = len(t)
    noise = rng.uniform(-1, 1, n)
    noise = noise - np.convolve(noise, np.ones(3) / 3, mode="same")
    return 0.7 * noise * env(n, 0.001, 0.07) + 0.4 * np.sin(2 * np.pi * 190 * t) * env(n, 0.001, 0.05)


def hat(length=0.08):
    n = int(length * RATE)
    noise = rng.uniform(-1, 1, n)
    noise = noise - np.convolve(noise, np.ones(2) / 2, mode="same")
    return 0.35 * noise * env(n, 0.0005, 0.02)


# --- Arranging --------------------------------------------------------------

class Track:
    def __init__(self, seconds):
        self.buf = np.zeros(int(seconds * RATE) + RATE)

    def add(self, at, sound, gain=1.0):
        i = int(at * RATE)
        end = min(len(self.buf), i + len(sound))
        self.buf[i:end] += gain * sound[: end - i]

    def render(self, seconds, level=0.6):
        b = self.buf[: int(seconds * RATE)]
        peak = np.max(np.abs(b)) or 1.0
        b = b / peak * level
        # No click at either end.
        fade = min(len(b) // 4, int(0.01 * RATE))
        b[:fade] *= np.linspace(0, 1, fade)
        b[-fade:] *= np.linspace(1, 0, fade)
        return b


def save(folder, name, samples):
    os.makedirs(os.path.join(OUT, folder), exist_ok=True)
    path = os.path.join(OUT, folder, name + ".wav")
    data = (np.clip(samples, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    print("%-10s %-14s %.1fs" % (folder, name, len(samples) / RATE))


# --- Ringtones: a few seconds each, with a rest at the end, since they loop -

def ringtones():
    # Marimba: a rolling figure over four chords.
    tr = Track(6)
    beat = 60 / 150 / 2
    bars = [(60, 67, 64, 67), (57, 67, 64, 67), (62, 69, 65, 69), (59, 67, 62, 67)]
    i = 0
    for rep in range(2):
        for bar in bars:
            for note in bar:
                tr.add(i * beat, marimba(note + 12))
                i += 1
    save("ringtones", "Marimba", tr.render(i * beat + 1.2))

    # Old Phone: a bell hammered twenty times a second, twice.
    tr = Track(4)
    for burst in (0.0, 1.2):
        for k in range(20):
            tr.add(burst + k * 0.05, bell(88 if k % 2 else 86, 0.3, 0.12), 0.6)
    save("ringtones", "Old Phone", tr.render(3.6))

    # Digital: four quick beeps, twice.
    tr = Track(3)
    for group in (0.0, 0.8):
        for k in range(4):
            tr.add(group + k * 0.1, square(96, 0.07, 0.5, 1.0))
    save("ringtones", "Digital", tr.render(2.4))

    # Xylophone: up a pentatonic scale and back.
    tr = Track(5)
    scale = [72, 74, 76, 79, 81, 84, 86, 88]
    run = scale + scale[-2::-1]
    for k, note in enumerate(run):
        tr.add(k * 0.11, xylophone(note))
    save("ringtones", "Xylophone", tr.render(len(run) * 0.11 + 1.0))

    # Harp: arpeggios swept up over two chords.
    tr = Track(6)
    at = 0.0
    for chord in ((60, 64, 67, 71, 72, 76, 79, 83), (53, 57, 60, 64, 65, 69, 72, 76)) * 2:
        for note in chord:
            tr.add(at, pluck(note, 1.6, 0.8, 0.998), 0.8)
            at += 0.07
        at += 0.35
    save("ringtones", "Harp", tr.render(at + 1.0))

    # Strum: a guitar strumming G, D, E minor, C.
    tr = Track(6)
    chords = [(43, 47, 50, 55, 59, 67), (50, 57, 62, 66), (40, 47, 52, 55, 59, 64), (48, 52, 55, 60, 64)]
    at = 0.0
    for chord in chords:
        for hit in (0.0, 0.5, 0.75):
            for k, note in enumerate(chord):
                tr.add(at + hit + k * 0.012, pluck(note, 1.0, 0.6, 0.995), 0.5)
        at += 1.0
    save("ringtones", "Strum", tr.render(at + 0.8))

    # Grove Beat: a slow street beat and a bass line.
    tr = Track(6)
    step = 60 / 92 / 4
    bass = [36, None, None, 36, None, None, 39, None, 41, None, None, 39, None, 36, None, None]
    for bar in range(2):
        base = bar * 16 * step
        for k in range(16):
            at = base + k * step
            if k in (0, 7, 10):
                tr.add(at, kick(), 1.0)
            if k in (4, 12):
                tr.add(at, snare(), 0.8)
            if k % 2 == 0:
                tr.add(at, hat(), 0.5)
            if bass[k] is not None:
                tr.add(at, square(bass[k], step * 2.5, 0.3, 0.3), 0.9)
    save("ringtones", "Grove Beat", tr.render(32 * step + 0.6))

    # Funk: a syncopated bass and a stabbed chord.
    tr = Track(6)
    step = 60 / 108 / 4
    riff = [40, None, 40, 52, None, 40, None, 50, None, 40, 43, None, 45, None, 47, None]
    for bar in range(2):
        base = bar * 16 * step
        for k, note in enumerate(riff):
            if note is not None:
                tr.add(base + k * step, pluck(note, 0.35, 0.9, 0.99), 1.0)
            if k in (4, 12):
                for n in (64, 67, 71):
                    tr.add(base + k * step, square(n, 0.12, 0.25, 0.08), 0.35)
            if k in (0, 8):
                tr.add(base + k * step, kick(), 0.7)
    save("ringtones", "Funk", tr.render(32 * step + 0.6))

    # Sonar: a ping and its echoes.
    tr = Track(4)
    for echo in range(4):
        tr.add(echo * 0.35, sine(1180, 1.2, 0.002, 0.35), 0.8 ** echo * (1 if echo == 0 else 0.5))
    tr.add(2.0, sine(1180, 1.2, 0.002, 0.35), 1.0)
    save("ringtones", "Sonar", tr.render(3.4))

    # Pulse: a synth arpeggio in A minor.
    tr = Track(5)
    notes = [57, 60, 64, 69, 64, 60] * 2 + [55, 59, 62, 67, 62, 59] * 2
    for k, note in enumerate(notes):
        tr.add(k * 0.1, square(note + 12, 0.12, 0.25 + 0.2 * (k % 3) / 3, 0.1))
    save("ringtones", "Pulse", tr.render(len(notes) * 0.1 + 0.8))

    # Trill: two notes shaken quickly, then an answer.
    tr = Track(4)
    at = 0.0
    for pair in ((79, 83), (81, 84)):
        for k in range(10):
            tr.add(at, glass(pair[k % 2], 0.4), 0.6)
            at += 0.06
        at += 0.3
    save("ringtones", "Trill", tr.render(at + 0.8))

    # Chimes: wind chimes, tuned to a pentatonic scale.
    tr = Track(6)
    scale = [72, 74, 76, 79, 81, 84, 86, 88, 91]
    at = 0.0
    while at < 4.0:
        tr.add(at, bell(int(rng.choice(scale)), 2.0, 0.9), rng.uniform(0.4, 1.0))
        at += rng.uniform(0.08, 0.35)
    save("ringtones", "Chimes", tr.render(5.2))

    # Night Drive: a slow electric-piano figure.
    tr = Track(7)
    at = 0.0
    for chord in ((57, 64, 67, 72), (53, 60, 64, 69), (55, 62, 65, 71), (52, 59, 64, 67)):
        for k, note in enumerate(chord):
            tr.add(at + k * 0.18, bell(note, 1.8, 0.8) + 0.4 * sine(hz(note), 1.8, 0.01, 0.8), 0.7)
        at += 1.1
    save("ringtones", "Night Drive", tr.render(at + 0.9))


# --- Text tones: a second or so ----------------------------------------------

def texttones():
    tr = Track(2)
    tr.add(0, bell(84, 1.2, 0.5))
    save("texttones", "Ding", tr.render(1.1))

    tr = Track(2)
    tr.add(0, glass(88, 0.8))
    tr.add(0.14, glass(84, 1.0))
    save("texttones", "Chime", tr.render(1.1))

    tr = Track(2)
    for k, note in enumerate((76, 79, 84)):
        tr.add(k * 0.11, marimba(note, 0.7))
    save("texttones", "Three Notes", tr.render(1.0))

    tr = Track(2)
    tr.add(0, bell(79, 1.6, 0.9))
    save("texttones", "Bell", tr.render(1.5))

    tr = Track(2)
    tr.add(0, glass(96, 1.2))
    save("texttones", "Glass", tr.render(1.0))

    t = t_axis(0.12)
    pop = np.sin(2 * np.pi * np.cumsum(900 - 5000 * t) / RATE) * env(len(t), 0.001, 0.03)
    save("texttones", "Pop", pop / np.max(np.abs(pop)) * 0.6)

    n = int(0.45 * RATE)
    noise = rng.uniform(-1, 1, n)
    out = np.zeros(n)
    # A noise sweep through a moving resonance.
    y1 = y2 = 0.0
    for i in range(n):
        f = 400 + 3200 * (i / n)
        r = 0.97
        c = 2 * r * np.cos(2 * np.pi * f / RATE)
        y = noise[i] * 0.1 + c * y1 - r * r * y2
        y2, y1 = y1, y
        out[i] = y
    out *= np.sin(np.pi * np.arange(n) / n)
    save("texttones", "Swoosh", out / np.max(np.abs(out)) * 0.6)

    tr = Track(1)
    tr.add(0, square(91, 0.06, 0.5, 1.0))
    tr.add(0.09, square(96, 0.08, 0.5, 1.0))
    save("texttones", "Blip", tr.render(0.3))

    tr = Track(2)
    tr.add(0, pluck(76, 0.8, 0.9, 0.997))
    tr.add(0.12, pluck(83, 1.0, 0.9, 0.997))
    save("texttones", "Note", tr.render(1.0))


if __name__ == "__main__":
    for folder in ("ringtones", "texttones"):
        path = os.path.join(OUT, folder)
        if os.path.isdir(path):
            for f in os.listdir(path):
                if f.endswith(".wav"):
                    os.remove(os.path.join(path, f))
    ringtones()
    texttones()
