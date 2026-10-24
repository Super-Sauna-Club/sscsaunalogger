#!/usr/bin/env python3
"""rp2040 -> sessions_extracted/. nutzt USB-CDC commands ?L + ?D <name>
auf /dev/ttyACM0. idempotent: ueberspringt files die schon lokal liegen.

protokoll (rp2040 sketch):
    ?L\n                         -> [SAUNA] LIST_BEGIN
                                    [SAUNA] FILE <name> <size>...
                                    [SAUNA] LIST_END
    ?D <name>\n                  -> [SAUNA] DUMP_BEGIN <name>
                                    <raw csv bytes>
                                    [SAUNA] DUMP_END <name>
"""
import argparse, os, re, sys, time
import serial


LIST_BEGIN = b"[SAUNA] LIST_BEGIN"
LIST_END   = b"[SAUNA] LIST_END"
FILE_RE    = re.compile(rb"\[SAUNA\] FILE (\S+) (\d+)")


def read_until(ser, sentinel, timeout):
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            if sentinel in buf:
                return bytes(buf)
        else:
            time.sleep(0.02)
    return bytes(buf)


def list_sessions(ser, timeout=8.0):
    ser.reset_input_buffer()
    ser.write(b"?L\n")
    ser.flush()
    data = read_until(ser, LIST_END, timeout)
    if LIST_END not in data:
        raise RuntimeError("kein LIST_END empfangen - rp2040 antwortet nicht")
    start = data.find(LIST_BEGIN)
    end   = data.find(LIST_END)
    if start < 0 or end < 0:
        return []
    region = data[start:end]
    out = []
    for m in FILE_RE.finditer(region):
        name = m.group(1).decode("ascii", errors="replace")
        size = int(m.group(2))
        out.append((name, size))
    return out


def dump_session(ser, name, expected_size, timeout=60.0):
    ser.reset_input_buffer()
    cmd = f"?D {name}\n".encode("ascii")
    ser.write(cmd)
    ser.flush()
    begin_marker = f"[SAUNA] DUMP_BEGIN {name}".encode("ascii")
    end_marker   = f"[SAUNA] DUMP_END {name}".encode("ascii")
    data = read_until(ser, end_marker, timeout)
    if end_marker not in data:
        raise RuntimeError(f"timeout: kein DUMP_END fuer {name}")
    bi = data.find(begin_marker)
    if bi < 0:
        raise RuntimeError(f"kein DUMP_BEGIN fuer {name}")
    body_start = data.find(b"\n", bi) + 1
    body_end   = data.find(end_marker)
    body = data[body_start:body_end]
    # sketch schickt \n vor DUMP_END -> trailing newline kappen
    if body.endswith(b"\r\n"):
        body = body[:-2]
    elif body.endswith(b"\n"):
        body = body[:-1]
    return body


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port",    default="/dev/ttyACM0")
    p.add_argument("--baud",    type=int, default=115200)
    p.add_argument("--out-dir", default="sessions_extracted")
    p.add_argument("--only",    default=None,
                   help="nur einen bestimmten session-namen ziehen")
    p.add_argument("--force",   action="store_true",
                   help="ueberschreibt files die schon existieren")
    args = p.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    print(f"connect {args.port} @ {args.baud}", file=sys.stderr)
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    # kurz warmlaufen lassen; rp2040 spuckt selber status-zeilen, die wollen wir nicht
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("listing sessions ...", file=sys.stderr)
    files = list_sessions(ser)
    if not files:
        print("device meldet keine sessions", file=sys.stderr)
        return 1

    print(f"{len(files)} session(s) auf SD:", file=sys.stderr)
    for n, s in files:
        print(f"  {n:<28} {s:>8} bytes", file=sys.stderr)

    new_count = 0
    skip_count = 0
    err_count  = 0
    for name, size in files:
        if args.only and name != args.only:
            continue
        out_path = os.path.join(args.out_dir, name)
        if not args.force and os.path.exists(out_path):
            existing = os.path.getsize(out_path)
            if existing >= size:
                print(f"  skip  {name} (lokal: {existing} bytes)", file=sys.stderr)
                skip_count += 1
                continue
            else:
                print(f"  grow  {name} (lokal {existing} < device {size})",
                      file=sys.stderr)
        print(f"  pull  {name} ({size} bytes) ...", file=sys.stderr, end="", flush=True)
        try:
            body = dump_session(ser, name, size, timeout=max(20.0, size / 1000.0))
        except Exception as e:
            print(f" FEHLER: {e}", file=sys.stderr)
            err_count += 1
            continue
        with open(out_path, "wb") as f:
            f.write(body)
        delta = len(body) - size
        marker = "" if delta == 0 else f" (delta {delta:+d})"
        print(f" ok, {len(body)} bytes{marker}", file=sys.stderr)
        new_count += 1

    ser.close()
    print(f"\n{new_count} neu geladen, {skip_count} skip, {err_count} fehler",
          file=sys.stderr)
    return 0 if err_count == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
