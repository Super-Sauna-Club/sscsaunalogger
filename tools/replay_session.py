#!/usr/bin/env python3
"""Spielt eine echte Session-CSV per USB-CDC (?F) in den RP2040 ein.

Der RP2040 geht in den SIM-Modus: statt des Fuehlers kommen die Werte
aus der Datei, mit beliebiger Geschwindigkeit. Detektor, Auto-Start,
Aufguss-/Lueften-Erkennung, CSV/JSON/EVT und die ESP32-UI laufen dabei
komplett echt. SIM-Sessions bekommen IDs mit X-Prefix (bzw. Y ohne Uhr).

Beispiele:
  python3 tools/replay_session.py sessions_extracted/S20260511_165849.csv --speed 20
  python3 tools/replay_session.py --synthetic --speed 30     # ganzer Abend mit Aufheizen

Das Skript wartet am Ende 8 s, damit der RP2040 den SIM-Modus beendet
(laufende SIM-Session wird dabei abgeschlossen).
"""
import argparse, math, sys, time
import serial


def load_csv(path):
    rows = []
    with open(path) as f:
        hdr = f.readline()
        v2 = hdr.startswith("t_ms")
        for line in f:
            p = line.strip().split(",")
            if len(p) < 3 or not p[1] or not p[2]:
                continue
            try:
                rows.append([float(p[0]), float(p[1]), float(p[2])])
            except ValueError:
                pass
    if not v2:
        dup = len(rows) > 2 and rows[1][0] == rows[0][0]
        for i, r in enumerate(rows):
            r[0] = i * 500.0 if dup else r[0] * 1000.0
    return rows


def synthetic():
    """Kalt -> Aufheizen -> 2 Gaenge mit je 3 Kellen + Lueften -> Abkuehlen."""
    def es(t):
        return 6.1121 * math.exp((18.678 - t / 234.5) * (t / (257.14 + t)))
    rows = []
    for s in range(0, 5 * 3600 * 2):
        tm = s / 120.0
        if tm < 15:
            T, ah = 18.0, 9.0
        elif tm < 65:
            T = min(85.0, 18 + (tm - 15) * 1.4); ah = 9 + (tm - 15) * 0.22
        elif tm < 195:
            g = (tm - 65) % 65
            if g < 30:
                T = 85 - ((g - 10) * 0.6 if 10 < g < 20 else (6 - (g - 20) * 0.1 if g >= 20 else 0))
                ah = 20 + g * 0.8
                for k in range(3):
                    p = 10 + k * 2.2
                    if p < g < p + 1.5:
                        ah += 30 * (1 - (g - p) / 1.5)
            elif g < 45:
                T = max(62.0, 79 - (g - 30)); ah = 18
            else:
                T = min(85.0, 64 + (g - 45) * 1.6); ah = 18 + (g - 45) * 0.2
        else:
            T = max(20.0, 85 - (tm - 195) * 0.45); ah = 12
        rh = min(100.0, ah * (273.15 + T) / 216.7 / es(T) * 100)
        rows.append([s * 500.0, T, rh])
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", nargs="?")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--speed", type=float, default=10.0)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    rows = synthetic() if a.synthetic else load_csv(a.csv)
    if not rows:
        print("keine Daten", file=sys.stderr); return 1
    s = serial.Serial(a.port, 115200, timeout=0)
    s.dtr = True
    time.sleep(0.3)
    s.reset_input_buffer()
    base = 3600000.0            # Sim-Zeit beginnt bei 1 h (Ringe fuellen)
    t0_wall = time.time()
    t0_data = rows[0][0]
    log = b""
    for i, (t, T, rh) in enumerate(rows):
        due = t0_wall + (t - t0_data) / 1000.0 / a.speed
        while True:
            now = time.time()
            if now >= due:
                break
            time.sleep(min(0.02, due - now))
        s.write(f"?F {int(base + t - t0_data)} {T:.2f} {rh:.2f}\n".encode())
        chunk = s.read(65536)
        if chunk:
            log += chunk
            if not a.quiet:
                for line in chunk.decode("utf-8", "replace").splitlines():
                    if any(k in line for k in ("event", "session", "SIM", "health", "RESUME", "!!!")):
                        print(line)
        if i % 2000 == 0 and not a.quiet:
            print(f"# {i}/{len(rows)}  t={(t - t0_data) / 60000:.1f} min", flush=True)
    s.write(b"?S\n")
    end = time.time() + 8
    while time.time() < end:
        chunk = s.read(65536)
        if chunk:
            for line in chunk.decode("utf-8", "replace").splitlines():
                if line.strip():
                    print(line)
        time.sleep(0.1)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
