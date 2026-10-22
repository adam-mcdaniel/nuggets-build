#!/usr/bin/env python3
"""Cluster the regions of an analysis run and predict whole-run time and
energy from the representative regions, measured in an instrumentation run.

    cluster.py analysis-output.csv [--measure energy-output.csv] [-k K]
               [--gpu-weight W] [--gpu-mode presence|counts|logcounts] [--no-gpu]
               [--seed S] [--sweep] [--allow-mismatch]

Clustering uses only the analysis run (CPU and GPU vectors). Time and energy
come from the instrumentation run (hooks/energy.c, same interval), whose
region r is assumed to be the analysis run's region r.

Each region's vector is its CPU basic-block vector (L1-normalized) followed by
its GPU vector from tapir: the call count of each bucketed library call
(--gpu-mode amount, default), divided by the largest per-region call total of
the run and times --gpu-weight, so regions making more calls differ from regions
making fewer. Older modes, normalised per region (they lose the amount):
presence (1/0 per call; the default until 2026-10-20), counts, logcounts. Vectors are reduced to 15 dimensions (SVD) and clustered
with k-means; region 0 (start of the ROI) is its own warm-up cluster. Each
cluster's representative is the region closest to its centroid.

Prediction: every region costs what its representative costs per IR
instruction, so  total = sum_r metric[rep(r)] * inst[r] / inst[rep(r)],
for wall time (wall_ns rows) and GPU energy (gpu_energy_uj rows) of the
instrumentation run, compared with that run's measured totals.

Without -k, k is chosen by SimPoint's rule (smallest k whose BIC reaches 90%
of the BIC range over k = 1..30). --sweep prints the prediction error for
every k. Writes selection.json next to the CSV.
"""
import argparse
import json
import math
import os
import sys

import numpy as np


def read(path):
    rows = {}
    for line in open(path):
        p = line.rstrip("\n").split(",")
        if len(p) < 3 or not p[1].isdigit():
            continue
        r = int(p[1])
        d = rows.setdefault(r, {})
        if p[0] == "gpu_bb_id":  # 64-bit tapir hashes: keep exact (floats would round them)
            d[p[0]] = [int(x) for x in p[2:] if x]
        elif p[0] == "gpu_bbv":
            d[p[0]] = [float(x) for x in p[2:] if x]
        else:
            d[p[0]] = [float(x) for x in p[3:] if x]
    n = max(rows) + 1
    return [rows.get(r, {}) for r in range(n)]


def features(regions, gpu_weight, use_gpu, gpu_mode="amount"):
    cpu_ids = sorted({int(b) for g in regions for b in g.get("bb_id", [])})
    cc = {b: i for i, b in enumerate(cpu_ids)}
    gpu_ids = sorted({int(b) for g in regions for b in g.get("gpu_bb_id", [])}) if use_gpu else []
    gc = {b: i for i, b in enumerate(gpu_ids)}
    X = np.zeros((len(regions), len(cpu_ids) + len(gpu_ids)))
    for i, g in enumerate(regions):
        for b, v in zip(g.get("bb_id", []), g.get("bbv", [])):
            X[i, cc[int(b)]] = v
        s = X[i, :len(cpu_ids)].sum()
        if s:
            X[i, :len(cpu_ids)] /= s
        if gpu_ids and g.get("gpu_bb_id"):
            v = np.zeros(len(gpu_ids))
            counts = g.get("gpu_bbv") or [1.0] * len(g["gpu_bb_id"])
            for b, cnt in zip(g["gpu_bb_id"], counts):
                v[gc[int(b)]] = {"presence": 1.0, "counts": cnt, "logcounts": math.log1p(cnt), "amount": cnt}[gpu_mode]
            if v.sum() > 0 and gpu_mode != "amount":
                X[i, len(cpu_ids):] = gpu_weight * v / v.sum()
            elif gpu_mode == "amount":
                X[i, len(cpu_ids):] = v
    if gpu_mode == "amount" and gpu_ids:
        # call counts per key, NOT normalised per region (so 64 GEMMs != 4 GEMMs); divided by
        # the largest per-region call total of the run, so the busiest region's GPU part sums
        # to gpu_weight and the GPU part is on the CPU part's 0..1 scale in every app
        G = X[:, len(cpu_ids):]
        if G.sum(1).max() > 0:
            X[:, len(cpu_ids):] = gpu_weight * G / G.sum(1).max()
    return X, len(cpu_ids), len(gpu_ids)


def reduce(X, dims=15):
    Xc = X - X.mean(0)
    U, S, _ = np.linalg.svd(Xc, full_matrices=False)
    d = min(dims, len(S))
    return U[:, :d] * S[:d]


def kmeans(X, k, seed=0, n_init=10, iters=100):
    rng = np.random.RandomState(seed)
    best = None
    for _ in range(n_init):
        C = [X[rng.randint(len(X))]]
        for _ in range(1, k):
            d2 = np.min(((X[:, None] - np.array(C)[None]) ** 2).sum(-1), 1)
            C.append(X[rng.choice(len(X), p=d2 / d2.sum())] if d2.sum() > 0 else X[rng.randint(len(X))])
        C = np.array(C)
        for _ in range(iters):
            lab = ((X[:, None] - C[None]) ** 2).sum(-1).argmin(1)
            new = np.array([X[lab == j].mean(0) if (lab == j).any() else C[j] for j in range(k)])
            if np.allclose(new, C):
                break
            C = new
        sse = ((X - C[lab]) ** 2).sum()
        if best is None or sse < best[0]:
            best = (sse, lab, C)
    return best[1], best[2]


def bic(X, lab, C):
    R, M = X.shape
    K = len(C)
    if R <= K:
        return -math.inf
    var = max(((X - C[lab]) ** 2).sum() / (R - K), 1e-12)
    ll = sum(n * math.log(n) - n * math.log(R) - n * M / 2 * math.log(2 * math.pi * var) - (n - 1) * M / 2
             for n in (int((lab == j).sum()) for j in range(K)) if n)
    return ll - ((K - 1) + M * K + 1) / 2 * math.log(R)


def select(Y, k, seed=0):
    """Cluster regions 1.. into k-1 clusters; region 0 is cluster 0.
    Representative = region nearest its cluster's centroid; near-ties (within
    1e-9 relative) go to the lowest region id, so the choice is reproducible."""
    lab, C = kmeans(Y[1:], max(1, k - 1), seed=seed)
    labels = np.concatenate([[0], lab + 1])
    reps = {}
    for c in sorted(set(labels)):
        m = np.where(labels == c)[0]
        d = ((Y[m] - Y[m].mean(0)) ** 2).sum(1)
        reps[int(c)] = int(m[np.nonzero(d <= d.min() * (1 + 1e-9) + 1e-30)[0][0]])
    return labels, reps


def predict(metric, inst, labels, reps):
    return sum(metric[reps[c]] * inst[r] / inst[reps[c]] for r, c in enumerate(labels))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="analysis-output.csv (clustering)")
    ap.add_argument("--measure", help="energy-output.csv from the instrumentation run (time and energy)")
    ap.add_argument("-k", type=int, default=None, help="number of clusters including the warm-up cluster")
    ap.add_argument("--gpu-weight", type=float, default=1.0)
    ap.add_argument("--no-gpu", action="store_true", help="cluster on CPU vectors only")
    ap.add_argument("--gpu-mode", default="amount", choices=["presence", "counts", "logcounts", "amount"])
    ap.add_argument("--seed", type=int, default=0, help="k-means seed")
    ap.add_argument("--allow-mismatch", action="store_true",
                    help="accept an instrumentation run whose regions differ from the analysis run")
    ap.add_argument("--sweep", action="store_true", help="prediction error for k = 2..30")
    a = ap.parse_args()

    regions = read(a.csv)
    n = len(regions)
    inst = np.array([max(g.get("region_inst", [1])[0], 1) for g in regions])
    meas = read(a.measure) if a.measure else regions
    if a.measure:
        bad = [r for r in range(min(n, len(meas)))
               if meas[r].get("region_inst") and regions[r].get("region_inst")
               and meas[r]["region_inst"][0] != regions[r]["region_inst"][0]]
        if len(meas) != n or bad:
            msg = ("the instrumentation run has %d regions, the analysis run %d; %d regions differ in IR instructions"
                   % (len(meas), n, len(bad)))
            if not a.allow_mismatch:
                sys.exit("error: " + msg + " (the runs must cover the same work; --allow-mismatch to override)")
            print("warning: " + msg)
    m = min(n, len(meas))
    wall_all = np.array([g.get("wall_ns", [0])[0] for g in meas]) * 1e-9
    energy_all = np.array([sum(g.get("gpu_energy_uj", [0])) for g in meas]) * 1e-6
    wall = np.zeros(n)
    energy = np.zeros(n)
    wall[:m], energy[:m] = wall_all[:m], energy_all[:m]
    if not wall_all.any():
        sys.exit("no wall_ns rows: pass --measure energy-output.csv from the instrumentation run")
    X, ncpu, ngpu = features(regions, a.gpu_weight, not a.no_gpu, a.gpu_mode)
    Y = reduce(X)
    print("%d regions, %d CPU basic blocks, %d GPU call signatures%s" % (
        n, ncpu, ngpu, "" if ngpu or a.no_gpu else " (no gpu_* rows: run with tapir preloaded)"))
    total_t, total_e = wall_all.sum(), energy_all.sum()
    print("measured (instrumentation run): %.4f s, %.2f J (GPU)" % (total_t, total_e))

    def errors(k):
        labels, reps = select(Y, k, a.seed)
        pt, pe = predict(wall, inst, labels, reps), predict(energy, inst, labels, reps)
        return labels, reps, pt, pe, 100 * (pt / total_t - 1), 100 * (pe / total_e - 1)

    if a.sweep:
        print("\n  k   time err   energy err")
        for k in range(2, min(30, n) + 1):
            e = errors(k)
            print("%3d   %+7.2f%%   %+7.2f%%" % (k, e[4], e[5]))
        print()

    k = a.k
    k_rule = "fixed" if k is not None else "bic90"
    if k is None:
        scores = []
        for kk in range(1, min(30, n - 1) + 1):
            lab, C = kmeans(Y[1:], kk)
            scores.append(bic(Y[1:], lab, C))
        lo, hi = min(scores), max(scores)
        k = 1 + next(i + 1 for i, s in enumerate(scores) if hi == lo or s - lo >= 0.9 * (hi - lo))
        print("k = %d (SimPoint BIC rule, plus the warm-up cluster)" % k)
    labels, reps, pt, pe, et, ee = errors(k)

    print("\ncluster  regions  inst share  rep   rep time   rep energy")
    for c, r in sorted(reps.items()):
        m = labels == c
        print("%7d  %7d  %9.1f%%  %4d  %8.4f s  %9.4f J" % (c, m.sum(), 100 * inst[m].sum() / inst.sum(), r, wall[r], energy[r]))
    cover = inst[list(reps.values())].sum() / inst.sum()
    print("\nrepresentatives: %d of %d regions (%.1f%% of IR instructions)" % (len(reps), n, 100 * cover))
    print("time:   predicted %.4f s, measured %.4f s, error %+.2f%%" % (pt, total_t, et))
    print("energy: predicted %.2f J,  measured %.2f J,  error %+.2f%%" % (pe, total_e, ee))

    out = os.path.join(os.path.dirname(os.path.abspath(a.csv)), "selection.json")
    json.dump({"k": k, "region_to_cluster": [int(x) for x in labels], "rep_rid": {str(c): r for c, r in reps.items()},
               "inst_counts": [int(x) for x in inst], "gpu_weight": a.gpu_weight, "use_gpu": not a.no_gpu,
               "gpu_mode": a.gpu_mode, "k_rule": k_rule, "seed": a.seed, "pca_dims": 15, "n_init": 10,
               "tapir_key": os.environ.get("TAPIR_KEY", "default (@dim:log2;@count:log2;@scalar:drop)"),
               "predicted_time_s": pt, "measured_time_s": float(total_t),
               "predicted_energy_j": pe, "measured_energy_j": float(total_e)},
              open(out, "w"), indent=1)
    print("wrote %s" % out)


if __name__ == "__main__":
    sys.exit(main())
