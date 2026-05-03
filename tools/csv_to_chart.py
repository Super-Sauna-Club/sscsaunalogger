#!/usr/bin/env python3
"""sauna-csv -> chart.png. dark-theme, warm-orange temperatur + cool-teal
luftfeuchte. fuer jedes CSV in --csv-dir wird ein PNG in --out-dir erstellt;
PNGs die schon existieren werden uebersprungen (idempotent).

CSV-format: t_elapsed_s,temp,rh,aufguss   (aufguss-spalte wird ignoriert)
filename:   S<YYYYMMDD>_<HHMMSS>.csv      (titel + sortierung)

optional companion json (gleicher basename):
    S20260427_180047.json
    {"name": "Thomas", "type": "session nur aufguss"}
ohne json: titel zeigt nur datum + uhrzeit.
"""
import argparse, datetime, glob, json, os, re, sys

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle


WEEKDAYS_DE = ["montag","dienstag","mittwoch","donnerstag","freitag",
               "samstag","sonntag"]
MONTHS_DE   = ["januar","februar","märz","april","mai","juni",
               "juli","august","september","oktober","november","dezember"]

BG_DEEP    = "#0c0c0e"
BG_PANEL   = "#16161a"
GRID       = "#262629"
TEXT       = "#e6e2d6"
TEXT_DIM   = "#8a8678"
TEXT_FAINT = "#5a5852"
T_COLOR    = "#ff7a3a"
T_FILL     = "#ff7a3a"
RH_COLOR   = "#5fc4d8"
RULE       = "#2a2a2e"

plt.rcParams.update({
    "font.family": ["Noto Sans", "DejaVu Sans", "sans-serif"],
    "font.size": 10,
    "axes.unicode_minus": False,
})


def parse_dt(filename):
    m = re.match(r"S(\d{8})_(\d{6})\.csv$", os.path.basename(filename))
    if not m:
        return None
    return datetime.datetime.strptime(f"{m.group(1)}{m.group(2)}",
                                      "%Y%m%d%H%M%S")


def fmt_de_date(dt):
    return f"{WEEKDAYS_DE[dt.weekday()]}, {dt.day}. {MONTHS_DE[dt.month-1]} {dt.year}"


def load_companion(csv_path):
    j = csv_path[:-4] + ".json"
    if not os.path.exists(j):
        return None
    try:
        with open(j) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def style_axes(ax):
    ax.set_facecolor(BG_PANEL)
    for s in ax.spines.values():
        s.set_color(RULE); s.set_linewidth(0.8)
    ax.tick_params(axis='both', colors=TEXT_DIM, labelsize=9.5,
                   length=4, width=0.7)
    ax.grid(True, color=GRID, linewidth=0.55, alpha=0.9)


def plot_session(df, dt, meta, out_path):
    t_min = df["t_min"].values
    temp  = df["temp"].values
    rh    = df["rh"].values

    fig = plt.figure(figsize=(13.5, 8.2), dpi=140, facecolor=BG_DEEP)
    gs = fig.add_gridspec(
        nrows=3, ncols=1,
        height_ratios=[1.5, 1.3, 6.5],
        hspace=0.42,
        left=0.07, right=0.95, top=0.96, bottom=0.06,
    )
    ax_head    = fig.add_subplot(gs[0, 0]); ax_head.set_facecolor(BG_DEEP); ax_head.axis("off")
    ax_metrics = fig.add_subplot(gs[1, 0]); ax_metrics.set_facecolor(BG_DEEP); ax_metrics.axis("off")
    ax         = fig.add_subplot(gs[2, 0])

    # ===== HEADER =====
    ax_head.text(0.0, 1.0, "SUPERSAUNA  CLUB",
                 transform=ax_head.transAxes,
                 color=T_COLOR, fontsize=10.0, weight="bold",
                 va="top", ha="left")
    ax_head.text(1.0, 1.0, dt.strftime("%H:%M") + "  uhr",
                 transform=ax_head.transAxes,
                 color=TEXT_DIM, fontsize=10.5, va="top", ha="right")
    ax_head.add_patch(Rectangle((0.0, 0.78), 0.05, 0.04,
                                transform=ax_head.transAxes,
                                facecolor=T_COLOR, edgecolor="none"))

    if meta and meta.get("name"):
        # mit companion: name oben gross, type-zeile darunter
        ax_head.text(0.0, 0.62, meta["name"],
                     transform=ax_head.transAxes,
                     color=TEXT, fontsize=27, weight="bold",
                     va="top", ha="left")
        sub = meta.get("type", "session")
        ax_head.text(0.0, 0.10, sub,
                     transform=ax_head.transAxes,
                     color=TEXT_DIM, fontsize=11.5, va="top", ha="left")
    else:
        # ohne companion: datum + uhrzeit als titel
        ax_head.text(0.0, 0.62, fmt_de_date(dt),
                     transform=ax_head.transAxes,
                     color=TEXT, fontsize=24, weight="bold",
                     va="top", ha="left")
        ax_head.text(0.0, 0.10, "session",
                     transform=ax_head.transAxes,
                     color=TEXT_DIM, fontsize=11.5, va="top", ha="left")
    ax_head.text(1.0, 0.10, fmt_de_date(dt),
                 transform=ax_head.transAxes,
                 color=TEXT_FAINT, fontsize=10.5, va="top", ha="right")

    # ===== PLOT =====
    style_axes(ax)
    t_lo = max(0.0, float(np.min(temp)) - 1.5)
    t_hi = float(np.max(temp)) + 1.5
    rh_lo = max(0.0, float(np.min(rh)) - 1.5)
    rh_hi = float(np.max(rh)) + 2.5

    ax.fill_between(t_min, t_lo, temp, color=T_FILL, alpha=0.13, linewidth=0)
    ax.plot(t_min, temp, color=T_COLOR, linewidth=2.4, alpha=0.97,
            solid_capstyle="round", zorder=4)
    ax.set_xlabel("zeit  (min)", fontsize=11, color=TEXT, labelpad=10)
    ax.set_ylabel("temperatur  (°C)", fontsize=11, color=T_COLOR,
                  weight="bold", labelpad=12)
    ax.tick_params(axis='y', labelcolor=T_COLOR)
    ax.set_ylim(t_lo, t_hi)
    ax.set_xlim(0, t_min[-1] if len(t_min) else 1)

    peak_idx = int(np.argmax(temp))
    ax.plot([t_min[peak_idx]], [temp[peak_idx]], 'o', color=T_COLOR,
            markersize=7, markeredgecolor=BG_DEEP, markeredgewidth=1.5,
            zorder=6)

    ax2 = ax.twinx()
    ax2.plot(t_min, rh, color=RH_COLOR, linewidth=1.9, alpha=0.94,
             solid_capstyle="round", zorder=3)
    ax2.set_ylabel("rel. luftfeuchte  (%)", fontsize=11, color=RH_COLOR,
                   weight="bold", labelpad=12)
    ax2.tick_params(axis='y', labelcolor=RH_COLOR, labelsize=9.5,
                    length=4, width=0.7)
    for s in ax2.spines.values():
        s.set_color(RULE)
    ax2.set_ylim(rh_lo, rh_hi)
    ax2.grid(False)

    # ===== METRICS =====
    duration = float(t_min[-1]) if len(t_min) else 0.0
    metrics = [
        ("dauer",       f"{duration:.1f} min"),
        ("peak-temp",   f"{float(np.max(temp)):.1f} °C"),
        ("mittel-temp", f"{float(np.mean(temp)):.1f} °C"),
        ("rh start",    f"{float(rh[0]):.1f} %"),
        ("rh peak",     f"{float(np.max(rh)):.1f} %"),
        ("samples",     f"{len(df)}"),
    ]
    n = len(metrics)
    for i, (label, value) in enumerate(metrics):
        x = (i + 0.5) / n
        ax_metrics.text(x, 0.95, label, transform=ax_metrics.transAxes,
                        color=TEXT_FAINT, fontsize=9.5, ha="center", va="top")
        ax_metrics.text(x, 0.55, value, transform=ax_metrics.transAxes,
                        color=TEXT, fontsize=15.5, weight="bold",
                        ha="center", va="top")
    for i in range(1, n):
        x = i / n
        ax_metrics.plot([x, x], [0.05, 1.0], color=RULE, linewidth=0.7,
                        transform=ax_metrics.transAxes)
    ax_metrics.plot([0.0, 1.0], [1.05, 1.05], color=RULE, linewidth=0.8,
                    transform=ax_metrics.transAxes, clip_on=False)

    plt.savefig(out_path, facecolor=BG_DEEP, dpi=150, bbox_inches="tight",
                pad_inches=0.30)
    plt.close(fig)


def process(csv_path, out_dir, force, min_bytes):
    base = os.path.splitext(os.path.basename(csv_path))[0]
    out_path = os.path.join(out_dir, base + ".png")
    if not force and os.path.exists(out_path):
        return ("skip", csv_path, out_path)

    sz = os.path.getsize(csv_path)
    if sz < min_bytes:
        return ("too_small", csv_path, None)

    dt = parse_dt(csv_path)
    if dt is None:
        return ("bad_name", csv_path, None)

    df = pd.read_csv(csv_path)
    if len(df) < 2 or "temp" not in df.columns or "rh" not in df.columns:
        return ("bad_csv", csv_path, None)
    df["t_min"] = df["t_elapsed_s"] / 60.0
    df = df.dropna(subset=["temp", "rh"])
    if len(df) < 2:
        return ("no_valid_samples", csv_path, None)

    meta = load_companion(csv_path)
    plot_session(df, dt, meta, out_path)
    return ("ok", csv_path, out_path)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--csv-dir", default="/tmp/sauna_csv",
                   help="ordner mit S*.csv files (default: %(default)s)")
    p.add_argument("--out-dir", default=None,
                   help="output ordner (default: <csv-dir>/charts)")
    p.add_argument("--force", action="store_true",
                   help="vorhandene PNGs ueberschreiben")
    p.add_argument("--min-bytes", type=int, default=1000,
                   help="CSVs kleiner als N bytes ignorieren (default: %(default)s)")
    args = p.parse_args()

    out_dir = args.out_dir or os.path.join(args.csv_dir, "charts")
    os.makedirs(out_dir, exist_ok=True)

    files = sorted(glob.glob(os.path.join(args.csv_dir, "S*.csv")))
    if not files:
        print(f"keine S*.csv files in {args.csv_dir}", file=sys.stderr)
        sys.exit(1)

    counts = {"ok": 0, "skip": 0, "too_small": 0, "bad_name": 0,
              "bad_csv": 0, "no_valid_samples": 0}
    for f in files:
        status, path, out = process(f, out_dir, args.force, args.min_bytes)
        counts[status] = counts.get(status, 0) + 1
        if status == "ok":
            print(f"  {os.path.basename(path):<28} -> {out}")
        elif status == "skip":
            print(f"  {os.path.basename(path):<28} (skip, png existiert)")
        else:
            print(f"  {os.path.basename(path):<28} ({status})")

    print(f"\n{counts['ok']} neu, {counts['skip']} skip, "
          f"{sum(counts.values()) - counts['ok'] - counts['skip']} ignoriert")


if __name__ == "__main__":
    main()
