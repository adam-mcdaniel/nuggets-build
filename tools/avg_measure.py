#!/usr/bin/env python3
"""Average several instrumentation runs (energy-output.csv files) region by region.

    avg_measure.py [--no-interpolate] [--median] OUT.csv RUN1.csv RUN2.csv ...

Regions are deterministic (same IR-instruction boundaries every run), so the
per-region wall time and GPU energy can be averaged across runs. This matters
when regions are shorter than the energy counter's update period (~6-8 ms on
MI210): the counter then moves in steps of several regions' energy, each step
landing in one region.

Interpolation (default): in each run, the counter is read at every region
boundary; between two boundaries where it changed, the GPU drew
(energy step)/(time between them), so each region gets that power times its
own duration (piecewise-constant power; before the first and after the last
step, the nearest interval's power). Then runs are averaged.
--median: per-region median over runs instead of the mean (robust to a single
region stalling in one run, as happens on a shared login node).
Rows other than wall_ns / gpu_energy_uj (e.g. region_inst) are taken from the
first run and must agree in all runs.
"""
import sys
from collections import OrderedDict


RAW_ROWS = ("t_end_ns", "gpu_counter_uj", "gpu_counter_ts")


def read(path):
    rows = OrderedDict()
    for line in open(path):
        p = line.rstrip("\n").split(",")
        if len(p) < 4 or not p[1].isdigit():
            continue
        rows[(p[0], int(p[1]), p[2])] = [float(x) for x in p[3:] if x]
    return rows


def interpolate(d):
    """Per-region energy from piecewise-constant power between counter steps."""
    n = 1 + max(k[1] for k in d)
    th = [k[2] for k in d if k[0] == "wall_ns"][0]
    t = [d[("wall_ns", r, th)][0] for r in range(n)]
    T = [0.0]
    for x in t:
        T.append(T[-1] + x)
    for key in [k for k in d if k[0] == "gpu_energy_uj" and k[1] == 0]:
        for c in range(len(d[key])):
            e = [d[("gpu_energy_uj", r, key[2])][c] for r in range(n)]
            E = [0.0]
            for x in e:
                E.append(E[-1] + x)
            knots = [0] + [k for k in range(1, n + 1) if e[k - 1] > 0]
            if len(knots) < 3:  # at most one step: uniform power
                P = [E[n] / T[n] if T[n] else 0.0] * n
            else:
                # power on each knot interval, extended to the ends
                seg = [(E[b] - E[a]) / (T[b] - T[a]) if T[b] > T[a] else 0.0
                       for a, b in zip(knots[1:], knots[2:])]
                P = []
                for r in range(n):
                    i = next((j for j, (a, b) in enumerate(zip(knots[1:], knots[2:])) if a <= r < b), None)
                    P.append(seg[0] if i is None and r < knots[1] else seg[-1] if i is None else seg[i])
            for r in range(n):
                d[("gpu_energy_uj", r, key[2])][c] = P[r] * t[r]
    return d


def main():
    args = sys.argv[1:]
    interp, median = True, False
    while args and args[0].startswith("--"):
        if args[0] == "--no-interpolate":
            interp = False
        elif args[0] == "--median":
            median = True
        else:
            sys.exit(__doc__)
        args = args[1:]
    out, runs = args[0], args[1:]
    if not runs:
        sys.exit(__doc__)
    data = [read(r) for r in runs]
    if interp:
        data = [interpolate(d) for d in data]
    # Drop disturbed runs (e.g. a stall on a shared node): total time above
    # 1.5x the median run.
    total = [sum(v[0] for k, v in d.items() if k[0] == "wall_ns") for d in data]
    med = sorted(total)[len(total) // 2]
    keep = [i for i, t in enumerate(total) if t <= 1.5 * med]
    for i in range(len(runs)):
        if i not in keep:
            print("dropped %s: %.3f s, median run %.3f s" % (runs[i], total[i] * 1e-9, med * 1e-9))
    runs, data = [runs[i] for i in keep], [data[i] for i in keep]
    base = data[0]
    for path, d in zip(runs[1:], data[1:]):
        if set(d) != set(base):
            sys.exit("%s: different regions than %s" % (path, runs[0]))
        for key in base:
            if key[0] == "region_inst" and d[key] != base[key]:
                sys.exit("%s: region %d has %s instructions, %s in %s" % (path, key[1], d[key], base[key], runs[0]))
    with open(out, "w") as f:
        f.write("type,region,thread,data\n")
        for key, v in base.items():
            if key[0] in RAW_ROWS:   # per-run raw readings: meaningless averaged, read them from the run files
                continue
            if key[0] in ("wall_ns", "gpu_energy_uj"):
                if median:
                    v = [sorted(d[key][i] for d in data)[len(data) // 2] if len(data) % 2 else
                         sum(sorted(d[key][i] for d in data)[len(data) // 2 - 1:len(data) // 2 + 1]) / 2
                         for i in range(len(v))]
                else:
                    v = [sum(d[key][i] for d in data) / len(data) for i in range(len(v))]
                vals = ",".join("%.0f" % x for x in v)
            else:
                vals = ",".join("%d" % x for x in v)
            f.write("%s,%d,%s,%s\n" % (key[0], key[1], key[2], vals))
    print("averaged (%s) %d runs, %d regions -> %s" % ("median" if median else "mean", len(runs), 1 + max(k[1] for k in base), out))


if __name__ == "__main__":
    main()
