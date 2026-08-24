#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
star-bionic profiler -- Stage 10 system-side sampler.

Samples CPU load and frequency, GPU load and frequency, thermals, memory and
throttling state at a fixed interval, and writes CSV plus a JSON summary.

Design rules, carried over from vkprobe:

  1. Every source is probed at startup and its availability recorded. A source
     that cannot be read is reported as unavailable, once, up front.
  2. An unreadable metric is written as an EMPTY CSV cell and null in JSON --
     never 0. A fabricated zero in a thermal or load trace is worse than a
     gap, because it silently drags an average down.
  3. CSV rows are flushed every sample. Android can kill a backgrounded
     Termux process at any time; a run that dies at minute 9 of 10 should
     still yield nine minutes of data.

Usage:
    ./tools/profiler.py --duration 120 --out runs/baseline
    ./tools/profiler.py --duration 300 --interval 1 --label "fh6 1080p high"

Run `termux-wake-lock` first if the game will be in the foreground, or
Android will suspend this process as soon as Termux loses focus.
"""

import argparse
import json
import os
import re
import signal
import sys
import time
from datetime import datetime, timezone

# ----------------------------------------------------------------------
# low-level reads -- every one returns None rather than raising or guessing
# ----------------------------------------------------------------------


def read_text(path):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except (OSError, UnicodeDecodeError):
        return None


def read_int(path):
    t = read_text(path)
    if t is None:
        return None
    m = re.search(r"-?\d+", t)
    return int(m.group()) if m else None


def glob_dirs(pattern_dir, name_re):
    try:
        rx = re.compile(name_re)
        return sorted(
            os.path.join(pattern_dir, d)
            for d in os.listdir(pattern_dir)
            if rx.fullmatch(d)
        )
    except OSError:
        return []


# ----------------------------------------------------------------------
# CPU
# ----------------------------------------------------------------------


class CpuLoad:
    """
    /proc/stat exposes cumulative jiffies, so a percentage only exists
    between two reads. The first sample therefore has no CPU load and is
    reported as null rather than as zero.
    """

    def __init__(self):
        self.prev = {}

    @staticmethod
    def _parse():
        out = {}
        txt = read_text("/proc/stat")
        if txt is None:
            return out
        for line in txt.splitlines():
            if not line.startswith("cpu"):
                continue
            parts = line.split()
            key = parts[0]
            try:
                vals = [int(v) for v in parts[1:]]
            except ValueError:
                continue
            if len(vals) < 5:
                continue
            idle = vals[3] + vals[4]          # idle + iowait
            total = sum(vals)
            out[key] = (total, idle)
        return out

    def sample(self):
        cur = self._parse()
        pct = {}
        for key, (total, idle) in cur.items():
            p = self.prev.get(key)
            if p is None:
                pct[key] = None
                continue
            dt, di = total - p[0], idle - p[1]
            # A non-advancing counter means no elapsed time to attribute, not
            # an idle CPU.
            pct[key] = round((dt - di) / dt * 100.0, 1) if dt > 0 else None
        self.prev = cur
        return pct


class CpuIdleLoad:
    """
    Fallback for when /proc/stat is unreadable, which recent Android does to
    apps. cpuidle exposes cumulative time spent in each idle state per core,
    so busy = elapsed wall time - elapsed idle time.

    An offline core is reported as None, not as 100% busy: its idle counter
    stops advancing while the wall clock does not, and big.LITTLE Android
    hotplugs cores constantly.
    """

    def __init__(self, cpudirs):
        self.dirs = cpudirs
        self.prev = {}
        self.available = any(self._idle_us(c) is not None for c in cpudirs)

    @staticmethod
    def _online(cdir):
        v = read_int(os.path.join(cdir, "online"))
        # cpu0 usually cannot be offlined and exposes no `online` file.
        return True if v is None else v == 1

    @staticmethod
    def _idle_us(cdir):
        base = os.path.join(cdir, "cpuidle")
        try:
            states = [d for d in os.listdir(base) if d.startswith("state")]
        except OSError:
            return None
        total = None
        for st in states:
            v = read_int(os.path.join(base, st, "time"))
            if v is not None:
                total = (total or 0) + v
        return total

    def sample(self):
        now = time.monotonic()
        out = {}
        for i, c in enumerate(self.dirs):
            key = f"cpu{i}"
            idle = self._idle_us(c) if self._online(c) else None
            prev = self.prev.get(key)
            if idle is None or prev is None:
                out[key] = None
            else:
                d_idle = (idle - prev[0]) / 1e6      # us -> s
                d_wall = now - prev[1]
                out[key] = (
                    round(min(100.0, max(0.0, (d_wall - d_idle) / d_wall * 100.0)), 1)
                    if d_wall > 0 else None
                )
            self.prev[key] = (idle, now) if idle is not None else None
        vals = [v for v in out.values() if v is not None]
        out["cpu"] = round(sum(vals) / len(vals), 1) if vals else None
        return out


def loadavg():
    t = read_text("/proc/loadavg")
    if not t:
        return None
    try:
        return float(t.split()[0])
    except (ValueError, IndexError):
        return None


def cpu_dirs():
    return glob_dirs("/sys/devices/system/cpu", r"cpu\d+")


def cpu_mhz(cdir):
    khz = read_int(os.path.join(cdir, "cpufreq/scaling_cur_freq"))
    return round(khz / 1000.0) if khz else None


# ----------------------------------------------------------------------
# GPU (Adreno / KGSL)
# ----------------------------------------------------------------------

KGSL_CANDIDATES = ["/sys/class/kgsl/kgsl-3d0", "/sys/class/kgsl/kgsl-2d0"]


def find_kgsl():
    for d in KGSL_CANDIDATES:
        if os.path.isdir(d):
            return d
    return None


class Gpu:
    def __init__(self):
        self.base = find_kgsl()
        self.busy_path = None
        self.clk_path = None
        self.temp_path = None
        self.throttle_path = None
        if not self.base:
            return
        for rel in ("gpu_busy_percentage", "devfreq/gpu_load", "gpu_busy"):
            p = os.path.join(self.base, rel)
            if read_text(p) is not None:
                self.busy_path = p
                break
        for rel in ("gpuclk", "devfreq/cur_freq", "clock_mhz"):
            p = os.path.join(self.base, rel)
            if read_text(p) is not None:
                self.clk_path = p
                break
        for rel in ("temp", "gpu_temp"):
            p = os.path.join(self.base, rel)
            if read_text(p) is not None:
                self.temp_path = p
                break
        p = os.path.join(self.base, "throttling")
        if read_text(p) is not None:
            self.throttle_path = p

    def busy_pct(self):
        return read_int(self.busy_path) if self.busy_path else None

    def mhz(self):
        v = read_int(self.clk_path) if self.clk_path else None
        if v is None:
            return None
        # gpuclk reports Hz; devfreq/cur_freq also Hz; clock_mhz already MHz.
        if v > 10_000_000:
            return round(v / 1_000_000)
        if v > 10_000:
            return round(v / 1000)
        return v

    def temp_c(self):
        v = read_int(self.temp_path) if self.temp_path else None
        if v is None:
            return None
        return round(v / 1000.0, 1) if abs(v) > 1000 else float(v)

    def throttling(self):
        return read_int(self.throttle_path) if self.throttle_path else None


# ----------------------------------------------------------------------
# Thermals
# ----------------------------------------------------------------------

# Samsung exposes dozens of zones, and taking simply the first N yields ten
# CPU cores and no battery or skin sensor at all. Group by what the zone is
# measuring and take a spread, so sustained-load behaviour is actually visible.
THERMAL_CATEGORIES = (
    ("gpu",     ("gpu",)),
    ("battery", ("batt",)),
    ("skin",    ("skin", "usb", "case", "quiet")),
    ("soc",     ("soc", "ap_therm", "qpnp", "pm8", "mdm")),
    ("cpu",     ("cpu",)),
)
PER_CATEGORY = 3


def thermal_zones(limit=12):
    buckets = {name: [] for name, _ in THERMAL_CATEGORIES}
    seen = set()
    for z in glob_dirs("/sys/class/thermal", r"thermal_zone\d+"):
        t = read_text(os.path.join(z, "type"))
        if not t or t in seen:
            continue
        temp_path = os.path.join(z, "temp")
        if read_int(temp_path) is None:
            continue
        low = t.lower()
        for cat, pats in THERMAL_CATEGORIES:
            if any(p in low for p in pats):
                buckets[cat].append((t, temp_path))
                seen.add(t)
                break

    out = []
    # Round-robin so no single category can crowd the others out.
    for rank in range(PER_CATEGORY):
        for cat, _ in THERMAL_CATEGORIES:
            if rank < len(buckets[cat]) and len(out) < limit:
                out.append(buckets[cat][rank])
    return out


def temp_c(path):
    v = read_int(path)
    if v is None:
        return None
    # Kernel zones report millidegrees; a few report whole degrees.
    return round(v / 1000.0, 1) if abs(v) > 1000 else float(v)


# ----------------------------------------------------------------------
# Memory
# ----------------------------------------------------------------------


def meminfo():
    txt = read_text("/proc/meminfo")
    if txt is None:
        return {}
    out = {}
    for line in txt.splitlines():
        m = re.match(r"(\w+):\s+(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))  # kB
    return out


# ----------------------------------------------------------------------
# Sampler
# ----------------------------------------------------------------------


class Profiler:
    def __init__(self, args):
        self.args = args
        self.cpus = cpu_dirs()
        self.cpu = CpuLoad()
        self.cpu_idle = CpuIdleLoad(self.cpus)
        # /proc/stat is preferred when readable; recent Android blocks it.
        self.cpu_source = (
            "/proc/stat" if read_text("/proc/stat") is not None
            else ("cpuidle" if self.cpu_idle.available else None)
        )
        self.gpu = Gpu()
        self.zones = thermal_zones()
        self.samples = []
        self.stop = False

    # -- availability, reported once up front -------------------------
    def availability(self):
        mi = meminfo()
        return {
            "cpu_load": self.cpu_source is not None,
            "cpu_load_source": self.cpu_source,
            "cpu_count": len(self.cpus),
            "cpu_freq": any(cpu_mhz(c) is not None for c in self.cpus),
            "kgsl_path": self.gpu.base,
            "gpu_busy": self.gpu.busy_path,
            "gpu_freq": self.gpu.clk_path,
            "gpu_temp": self.gpu.temp_path,
            "gpu_throttling": self.gpu.throttle_path,
            "thermal_zones": [z[0] for z in self.zones],
            "memory": bool(mi),
            # Deliberately absent: see FPS note in the summary and README.
            "fps": False,
            "frametime": False,
            "shader_compilation": False,
            "pipeline_creation": False,
        }

    def columns(self):
        cols = ["t_iso", "t_rel_s", "cpu_total_pct"]
        cols += [f"cpu{i}_pct" for i in range(len(self.cpus))]
        cols += [f"cpu{i}_mhz" for i in range(len(self.cpus))]
        cols += ["gpu_busy_pct", "gpu_mhz", "gpu_temp_c", "gpu_throttling"]
        cols += [self._zone_col(n) for n, _ in self.zones]
        cols += ["loadavg_1m", "mem_avail_mb", "mem_used_mb", "swap_used_mb"]
        return cols

    @staticmethod
    def _zone_col(name):
        return "temp_" + re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").lower() + "_c"

    def sample(self, t0):
        pct = (self.cpu.sample() if self.cpu_source == "/proc/stat"
               else self.cpu_idle.sample() if self.cpu_source else {})
        mi = meminfo()

        total_kb = mi.get("MemTotal")
        avail_kb = mi.get("MemAvailable")
        swap_total = mi.get("SwapTotal")
        swap_free = mi.get("SwapFree")

        row = {
            "t_iso": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "t_rel_s": round(time.monotonic() - t0, 2),
            "cpu_total_pct": pct.get("cpu"),
        }
        for i in range(len(self.cpus)):
            row[f"cpu{i}_pct"] = pct.get(f"cpu{i}")
        for i, c in enumerate(self.cpus):
            row[f"cpu{i}_mhz"] = cpu_mhz(c)

        row["gpu_busy_pct"] = self.gpu.busy_pct()
        row["gpu_mhz"] = self.gpu.mhz()
        row["gpu_temp_c"] = self.gpu.temp_c()
        row["gpu_throttling"] = self.gpu.throttling()

        for name, path in self.zones:
            row[self._zone_col(name)] = temp_c(path)

        row["loadavg_1m"] = loadavg()
        row["mem_avail_mb"] = round(avail_kb / 1024) if avail_kb else None
        row["mem_used_mb"] = (
            round((total_kb - avail_kb) / 1024) if total_kb and avail_kb else None
        )
        row["swap_used_mb"] = (
            round((swap_total - swap_free) / 1024)
            if swap_total is not None and swap_free is not None
            else None
        )
        return row

    def run(self):
        args = self.args
        cols = self.columns()
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        csv_path = args.out + ".csv"
        json_path = args.out + ".json"

        avail = self.availability()
        self._print_availability(avail)

        t0 = time.monotonic()
        deadline = t0 + args.duration if args.duration else None

        with open(csv_path, "w") as cf:
            cf.write(",".join(cols) + "\n")
            cf.flush()
            n = 0
            while not self.stop:
                if deadline and time.monotonic() >= deadline:
                    break
                tick = time.monotonic()
                row = self.sample(t0)
                self.samples.append(row)
                # Empty cell, never 0, for anything unreadable.
                cf.write(
                    ",".join("" if row.get(c) is None else str(row[c]) for c in cols)
                    + "\n"
                )
                cf.flush()
                n += 1
                if not args.quiet:
                    self._print_live(row, n)
                # Drift-free pacing: sleep the remainder of the interval.
                rest = args.interval - (time.monotonic() - tick)
                if rest > 0:
                    time.sleep(rest)

        summary = self.summarize(avail)
        with open(json_path, "w") as jf:
            json.dump(
                {
                    "label": args.label,
                    "interval_s": args.interval,
                    "samples_taken": len(self.samples),
                    "availability": avail,
                    "summary": summary,
                    "samples": self.samples,
                },
                jf,
                indent=2,
            )

        print(f"\nwrote {csv_path}  ({len(self.samples)} samples)")
        print(f"wrote {json_path}")
        self._print_summary(summary)

    # -- output -------------------------------------------------------
    @staticmethod
    def _print_availability(a):
        print("=" * 62)
        print("star-bionic profiler -- source availability")
        print("=" * 62)
        print(f"  cpu cores            {a['cpu_count']}")
        print(f"  cpu load             {a['cpu_load_source'] or 'NO SOURCE'}")
        print(f"  cpu frequency        {'yes' if a['cpu_freq'] else 'NO'}")
        print(f"  kgsl                 {a['kgsl_path'] or 'NOT FOUND'}")
        for k, label in (
            ("gpu_busy", "gpu load"),
            ("gpu_freq", "gpu frequency"),
            ("gpu_temp", "gpu temperature"),
            ("gpu_throttling", "gpu throttling"),
        ):
            print(f"  {label:20} {a[k] or 'NOT READABLE'}")
        print(f"  thermal zones        {len(a['thermal_zones'])}: "
              f"{', '.join(a['thermal_zones']) or 'none'}")
        print(f"  memory               {'yes' if a['memory'] else 'NO'}")
        print("  fps / frametime      NOT AVAILABLE -- needs in-container capture")
        print("=" * 62)

    @staticmethod
    def _print_live(row, n):
        def f(v, w, suf=""):
            return f"{v}{suf}".rjust(w) if v is not None else "-".rjust(w)

        sys.stdout.write(
            f"\r[{n:5d}] cpu {f(row.get('cpu_total_pct'), 5, '%')}  "
            f"gpu {f(row.get('gpu_busy_pct'), 4, '%')} "
            f"{f(row.get('gpu_mhz'), 5, 'MHz')}  "
            f"gputemp {f(row.get('gpu_temp_c'), 6, 'C')}  "
            f"mem {f(row.get('mem_used_mb'), 6, 'MB')}   "
        )
        sys.stdout.flush()

    def summarize(self, avail):
        out = {}
        if not self.samples:
            return out
        for col in self.columns():
            if col in ("t_iso", "t_rel_s"):
                continue
            vals = [
                s[col] for s in self.samples if s.get(col) is not None
            ]
            if not vals:
                # Distinguish "nothing readable" from "read as zero".
                out[col] = None
                continue
            vals_sorted = sorted(vals)
            out[col] = {
                "n": len(vals),
                "min": vals_sorted[0],
                "max": vals_sorted[-1],
                "mean": round(sum(vals) / len(vals), 1),
                "p50": vals_sorted[len(vals_sorted) // 2],
                "p95": vals_sorted[min(len(vals_sorted) - 1,
                                       int(len(vals_sorted) * 0.95))],
            }
        return out

    @staticmethod
    def _print_summary(summary):
        interesting = [
            "cpu_total_pct", "loadavg_1m", "gpu_busy_pct", "gpu_mhz",
            "gpu_temp_c", "mem_used_mb",
        ]
        rows = [(k, summary.get(k)) for k in interesting if summary.get(k)]
        rows += [
            (k, v) for k, v in summary.items()
            if k.startswith("temp_") and v
        ]
        if not rows:
            print("\nno metric produced a single readable sample.")
            return
        print(f"\n{'metric':26} {'min':>8} {'mean':>8} {'p95':>8} {'max':>8}")
        print("-" * 62)
        for k, v in rows:
            print(f"{k:26} {v['min']:>8} {v['mean']:>8} {v['p95']:>8} {v['max']:>8}")


def main():
    ap = argparse.ArgumentParser(
        description="star-bionic Stage 10 system profiler",
        epilog="Run termux-wake-lock first if the game will hold the foreground.",
    )
    ap.add_argument("--duration", type=float, default=0,
                    help="seconds to sample (0 = until Ctrl-C)")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="seconds between samples (default 1.0)")
    ap.add_argument("--out", default="profile",
                    help="output path prefix; writes .csv and .json")
    ap.add_argument("--label", default="",
                    help="free-text label recorded in the JSON")
    ap.add_argument("--quiet", action="store_true",
                    help="no live line")
    args = ap.parse_args()

    p = Profiler(args)

    def onsig(_sig, _frm):
        p.stop = True

    signal.signal(signal.SIGINT, onsig)
    signal.signal(signal.SIGTERM, onsig)

    p.run()


if __name__ == "__main__":
    main()
