#!/usr/bin/env python3
"""Summarize experiment CSVs into Markdown tables (median over repetitions).

Usage: summarize.py CSV [CSV ...] --baseline NAME [--stage1-decision]
NAME is a config (B0, CA, ...) or a stage-1 placement (P1, P2, ...).
"""

import argparse
import csv
import statistics
from collections import defaultdict

METRICS = ["qps", "p50", "p99", "p999"]
GROUP = ["stage", "scenario", "payload", "io", "cpu", "conns"]


def label(row):
    return row["placement"] or row["config"]


def load(paths):
    rows = []
    for p in paths:
        with open(p, newline="") as f:
            rows.extend(csv.DictReader(f))
    return rows


def medians(rows):
    """{(group..., load_pct, label): {metric: median, "ok": bool}}

    ok is False when any repetition had failures or achieved < 98% of its
    target QPS (its percentiles then describe an overloaded run).
    """
    acc = defaultdict(lambda: defaultdict(list))
    ok = defaultdict(lambda: True)
    for r in rows:
        key = tuple(r[g] for g in GROUP) + (r["load_pct"], label(r))
        for m in METRICS:
            if r.get(m):
                acc[key][m].append(float(r[m]))
        target = float(r.get("target_qps") or 0)
        if int(r.get("fail") or 0) > 0 or (
                target and float(r.get("qps") or 0) < 0.98 * target):
            ok[key] = False
    out = {k: {m: statistics.median(v) for m, v in ms.items()}
           for k, ms in acc.items()}
    for k in out:
        out[k]["ok"] = ok[k]
    return out


def delta(value, base):
    return (value - base) / base * 100.0 if base else float("nan")


def render(med, baseline):
    out = []
    groups = sorted({k[:len(GROUP)] for k in med},
                    key=lambda g: tuple(str(x) for x in g))
    for g in groups:
        out.append("### " + " ".join(f"{n}={v}" for n, v in zip(GROUP, g)))
        out.append("")
        out.append("| load | config | QPS | P50 us | P99 us | P999 us | "
                   "ΔP99 vs " + baseline + " | ok |")
        out.append("|---|---|---|---|---|---|---|---|")
        keys = sorted((k for k in med if k[:len(GROUP)] == g),
                      key=lambda k: (float(k[-2]), k[-1]))
        for k in keys:
            m = med[k]
            base = med.get(k[:-1] + (baseline,), {}).get("p99")
            d = f"{delta(m['p99'], base):+.1f}%" if base else "-"
            out.append(f"| {k[-2]} | {k[-1]} | {m['qps']:.0f} | "
                       f"{m['p50']:.0f} | {m['p99']:.0f} | {m['p999']:.0f} | "
                       f"{d} | {'' if m['ok'] else 'OVERLOAD'} |")
        out.append("")
    return "\n".join(out)


def stage1_decision(med, threshold):
    """GO if P2 is >= threshold% worse than P1 at any load, else STOP."""
    deltas = []
    for k, m in med.items():
        if k[-1] != "P2":
            continue
        base = med.get(k[:-1] + ("P1",), {}).get("p99")
        if base:
            deltas.append(delta(m["p99"], base))
    if not deltas:
        return "DECISION: UNKNOWN (no P1/P2 pairs)"
    worst = max(deltas)
    verdict = "GO" if worst >= threshold else "STOP"
    return f"DECISION: {verdict} (max ΔP99(P2 vs P1) = {worst:.1f}%)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--stage1-decision", action="store_true")
    ap.add_argument("--threshold", type=float, default=5.0)
    args = ap.parse_args()
    med = medians(load(args.csv))
    print(render(med, args.baseline))
    if args.stage1_decision:
        print(stage1_decision(med, args.threshold))


if __name__ == "__main__":
    main()
