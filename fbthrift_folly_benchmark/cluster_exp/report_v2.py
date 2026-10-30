#!/usr/bin/env python3
"""Every number of the scheduler v2 report, as Markdown, from one run of
run_v2.sh.

Usage: report_v2.py RUN_DIR
RUN_DIR holds the parts S1, S2, S1spe, S2spe, S1tm, S2tm, S1spe_tm, S2spe_tm,
S1sweep, S1imb, S2sweep, S2imb (each with stage2.csv; SPE parts also with
per-point spe.json and client.log; *tm parts may hold per-point
pidstat.txt for the server CPU). Missing parts are skipped. Values are
medians over repetitions; a point that missed 98% of its target QPS or had
failures in any repetition is OVL.
"""

import collections
import csv
import json
import pathlib
import re
import statistics
import sys

# ClusterAware2BlockingQueue::kWakeBucketUpperNs; the last bucket is open.
WAKE_BUCKET_UPPER_NS = [500, 1000, 2000, 4000, 8000, 16000, 32000, 64000,
                        128000, 256000]
CONFIG_ORDER = ["B0", "B1", "B2", "K1", "CA", "CA2", "CA2-G", "ST", "ST-P",
                "EB"]
NEW = ["CA2", "CA2-G"]
# Columns as in the v1 report: "other cluster" is peer_cluster; "other
# socket" (remote_socket + remote_mem) is the TCP traffic from the client on
# the other socket, the same for every configuration.
SPE_CLASSES = ["peer_cpu", "l3", "peer_cluster", "remote_socket", "remote_mem"]
# top_functions["cross_cluster"] counts samples of these classes.
TOP_CROSS = ["peer_cluster", "remote_socket"]
# Cross-cluster SPE samples by function, first match wins.
FUNCTION_GROUPS = [
    ("tm", re.compile(r"getThreadManager_deprecated")),
    ("queue", re.compile(r"ClusterAware2?BlockingQueue|UnboundedQueue|"
                         r"LifoSem|PriorityUnboundedQueueSet")),
    ("wake", re.compile(r"Baton|futex|\[kernel\.kallsyms\]")),
]
# Design section 3: the S2 600k sweeps where CA was worse than B0.
COUNTEREXAMPLES = ["p64_io48_cpu144_c192", "p1024_io24_cpu168_c96",
                   "p1024_io96_cpu96_c384"]
TOLERANCE = 1.03


def bucket_percentile(counts, q):
    """Upper bound (ns) of the bucket holding quantile q, -1 for the open
    last bucket, None without samples."""
    total = sum(counts)
    if total == 0:
        return None
    rank = max(1, -(-int(q * total * 1000) // 1000))  # ceil(q * total)
    seen = 0
    for i, n in enumerate(counts):
        seen += n
        if seen >= rank:
            return WAKE_BUCKET_UPPER_NS[i] if i < len(WAKE_BUCKET_UPPER_NS) else -1
    return -1


def parse_stats(line):
    """'k=v k=1,2' -> {'k': 'v', ...}; comma lists become lists of ints."""
    out = {}
    for tok in (line or "").split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k] = [int(x) for x in v.split(",")] if "," in v else v
    return out


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def fmt(v, digits=0):
    if v is None:
        return "—"
    return f"{v:.{digits}f}"


def pct(a, b):
    return "—" if a is None or b is None or not b else f"{100.0 * (a - b) / b:+.1f}%".replace("+-", "-")


def ok(rows):
    return all(int(float(r["fail"] or 0)) == 0 and
               float(r["qps"]) >= 0.98 * float(r["target_qps"]) for r in rows)


def group_key(r):
    return f"p{r['payload']}_io{r['io']}_cpu{r['cpu']}_c{r['conns']}"


def read_part(run, name):
    path = run / name / "stage2.csv"
    if not path.exists():
        return {}
    points = collections.defaultdict(list)
    with path.open(newline="") as f:
        for r in csv.DictReader(f):
            points[(group_key(r), int(r["load_pct"]), r["config"])].append(r)
    return points


def summarize(points):
    """{(group, load, config): {"ok", "p50", "p99", "rows"}}"""
    out = {}
    for key, rows in points.items():
        good = ok(rows)
        out[key] = dict(ok=good, rows=rows,
                        p50=med(float(r["p50"]) for r in rows),
                        p99=med(float(r["p99"]) for r in rows) if good else None)
    return out


def rank(config):
    return CONFIG_ORDER.index(config) if config in CONFIG_ORDER else len(CONFIG_ORDER)


def ordered(configs):
    return sorted(configs, key=rank)


def latency_tables(title, summary):
    groups = sorted({g for g, _, _ in summary})
    for g in groups:
        print(f"### {title} {g}\n")
        print("| load | config | p50 | p99 | vs B0 | vs CA |")
        print("|---|---|---|---|---|---|")
        loads = sorted({l for gg, l, _ in summary if gg == g})
        for load in loads:
            here = {c: v for (gg, l, c), v in summary.items()
                    if gg == g and l == load}
            b0 = here.get("B0", {}).get("p99")
            ca = here.get("CA", {}).get("p99")
            for c in ordered(here):
                v = here[c]
                p99 = fmt(v["p99"]) if v["ok"] else "OVL"
                print(f"| {load} | {c} | {fmt(v['p50'])} | {p99} | "
                      f"{pct(v['p99'], b0) if c != 'B0' else '—'} | "
                      f"{pct(v['p99'], ca) if c != 'CA' else '—'} |")
        print()


def queue_point(row):
    s = parse_stats(row.get("queue_stats", ""))
    if "local" not in s and row.get("local_takes"):
        # Runs before the queue_stats column: v1 queue counts only.
        s = dict(local=row["local_takes"], stolen=row["stolen_takes"])
    if "local" not in s:
        return None
    local, stolen = int(s["local"]), int(s["stolen"])
    takes = local + stolen
    out = dict(steal=stolen / takes if takes else None)
    if "assign_local" not in s:
        return out  # v1 queue: steal ratio only
    assigns = [int(s[k]) for k in ("assign_local", "assign_numa", "assign_far")]
    total = sum(assigns)
    per_k = (lambda n: 1000.0 * int(n) / takes) if takes else (lambda n: None)
    orphan_tries = int(s["orphan_take"]) + int(s["orphan_miss"])
    grab_tries = int(s["grab_take"]) + int(s["grab_miss"])
    out.update(
        local=assigns[0] / total if total else None,
        numa=assigns[1] / total if total else None,
        far=assigns[2] / total if total else None,
        orphan_add=per_k(s["orphan_add"]),
        orphan_miss=int(s["orphan_miss"]) / orphan_tries if orphan_tries else None,
        empty_target=int(s["empty_target"]) / total if total else None,
        handoff=per_k(s["handoff"]),
        grab_hit=int(s["grab_take"]) / grab_tries if grab_tries else None,
        grab_skip=per_k(s["grab_skip"]))
    return out


def pooled_wake(rows, key):
    """Bucket counts of `key` summed over the repetitions."""
    total = None
    for r in rows:
        b = parse_stats(r.get("queue_stats", "")).get(key)
        if isinstance(b, list):
            total = b if total is None else [x + y for x, y in zip(total, b)]
    return total or []


def us(ns):
    if ns is None:
        return "—"
    return ">256" if ns < 0 else f"{ns / 1000:g}"


def share(v):
    return "—" if v is None else f"{100 * v:.0f}%"


def queue_table(parts):
    print("| scenario | load | config | steal | assign local | same NUMA | "
          "other NUMA | orphan add /1k | orphan miss | empty target | "
          "handoff /1k | grab hit | grab skip /1k | wake local p50/p99 µs | "
          "wake remote p50/p99 µs |")
    print("|" + "---|" * 15)
    for scenario, summary in parts:
        for (g, load, c), v in sorted(summary.items(),
                                      key=lambda kv: (kv[0][0], kv[0][1],
                                                      rank(kv[0][2]))):
            if c not in ("CA",) + tuple(NEW):
                continue
            pts = [p for p in (queue_point(r) for r in v["rows"]) if p]
            if not pts:
                continue
            m = {k: med(p.get(k) for p in pts) for k in pts[0]}
            for key in ("wake_local", "wake_remote"):
                buckets = pooled_wake(v["rows"], key)
                m[key + "_p50"] = bucket_percentile(buckets, 0.5)
                m[key + "_p99"] = bucket_percentile(buckets, 0.99)
            if c == "CA":
                print(f"| {scenario} | {load} | {c} | {fmt(m['steal'], 2)} |"
                      + " — |" * 11)
                continue
            print(f"| {scenario} | {load} | {c} | {fmt(m['steal'], 2)} | "
                  f"{share(m['local'])} | {share(m['numa'])} | {share(m['far'])} | "
                  f"{fmt(m['orphan_add'], 1)} | {share(m['orphan_miss'])} | "
                  f"{share(m['empty_target'])} | {fmt(m['handoff'], 2)} | "
                  f"{share(m['grab_hit'])} | {fmt(m['grab_skip'], 1)} | "
                  f"{us(m['wake_local_p50'])}/{us(m['wake_local_p99'])} | "
                  f"{us(m['wake_remote_p50'])}/{us(m['wake_remote_p99'])} |")
    print()


def client_result(point_dir):
    for line in (point_dir / "client.log").read_text(errors="replace").splitlines():
        if line.startswith("RESULT "):
            return {k: float(v) for k, v in
                    (t.split("=", 1) for t in line.split()[1:] if "=" in t)}
    return {}


def spe_part(run, name):
    """{(load, config): {class: per-request loads, "cross", group shares,
    "others": Counter}} (medians over repetitions)."""
    points = collections.defaultdict(list)
    for spe in sorted((run / name).glob("*/*/load*_rep*/spe.json")):
        d = json.loads(spe.read_text())
        res = client_result(spe.parent)
        if not res.get("qps"):
            continue
        load = int(spe.parent.name.split("_rep")[0][4:])
        config = spe.parent.parent.name
        scale = d["period"] / (res["qps"] * d["secs"])
        c = d["classes"]
        row = {k: c[k]["n"] * scale for k in SPE_CLASSES}
        row["other_socket"] = row["remote_socket"] + row["remote_mem"]
        cross_n = sum(c[k]["n"] for k in TOP_CROSS)
        groups = collections.Counter()
        others = collections.Counter()
        for sym, n in d["top_functions"]["cross_cluster"]:
            for gname, rx in FUNCTION_GROUPS:
                if rx.search(sym):
                    groups[gname] += n
                    break
            else:
                others[re.sub(r"\s*\(.*\)$", "", sym)] += n
        for gname, _ in FUNCTION_GROUPS:
            row[gname] = groups[gname] / cross_n if cross_n else None
            row[gname + "_per_req"] = groups[gname] * scale
        row["others"] = {s: n / cross_n for s, n in others.items()} if cross_n else {}
        row["p99"] = res.get("p99")
        points[(load, config)].append(row)
    out = {}
    for key, rows in points.items():
        m = {k: med(r[k] for r in rows) for k in rows[0] if k != "others"}
        merged = collections.Counter()
        for r in rows:
            merged.update(r["others"])
        m["others"] = {s: v / len(rows) for s, v in merged.most_common(8)}
        out[key] = m
    return out


def spe_tables(run):
    result = {}
    for name in ("S1spe", "S2spe"):
        part = spe_part(run, name)
        if not part:
            continue
        result[name] = part
        print(f"### {name}: loads per request beyond the core\n")
        print("Function columns use the top 30 functions per point, so they "
              "are lower bounds.\n")
        print("Loads per request; the queue, wake and tm columns count loads "
              "from another cluster's or socket's cache in those functions "
              "(per request, then their share of all such loads).\n")
        print("| load | config | p99 | peer cpu | own L3 | other cluster | "
              "other socket | queue /req | queue | wake/kernel /req | "
              "wake/kernel | tm /req | tm |")
        print("|" + "---|" * 13)
        for (load, c) in sorted(part, key=lambda k: (k[0], rank(k[1]))):
            m = part[(load, c)]
            print(f"| {load} | {c} | {fmt(m['p99'])} | {fmt(m['peer_cpu'], 1)} | "
                  f"{fmt(m['l3'], 1)} | {fmt(m['peer_cluster'], 1)} | "
                  f"{fmt(m['other_socket'], 1)} | {fmt(m['queue_per_req'], 1)} | "
                  f"{share(m['queue'])} | {fmt(m['wake_per_req'], 1)} | "
                  f"{share(m['wake'])} | {fmt(m['tm_per_req'], 1)} | "
                  f"{share(m['tm'])} |")
        print("\nOther functions with cross-cluster loads (share of them):\n")
        for (load, c) in sorted(part, key=lambda k: (k[0], rank(k[1]))):
            top = ", ".join(f"{s[:60]} {100 * v:.0f}%"
                            for s, v in part[(load, c)]["others"].items())
            print(f"- {load} {c}: {top}")
        print()
    return result


TM = "+TM"
PIDSTAT_TIME = re.compile(r"\d\d:\d\d:\d\d$")


def server_cpu(run, name):
    """{(load, config): (user, kernel)} in cores: press_server's pidstat
    samples averaged within a point, then the median over repetitions."""
    reps = collections.defaultdict(list)
    for path in sorted((run / name).glob("*/*/load*_rep*/pidstat.txt")):
        samples = []
        for line in path.read_text().splitlines():
            f = line.split()
            # Time UID PID %usr %system %guest %wait %CPU CPU Command
            if len(f) > 9 and PIDSTAT_TIME.match(f[0]) and "press_server" in f[9]:
                samples.append((float(f[3]), float(f[4])))
        if samples:
            load = int(path.parent.name.split("_rep")[0][4:])
            reps[(load, path.parent.parent.name)].append(
                tuple(statistics.mean(s[i] for s in samples) / 100 for i in (0, 1)))
    return {k: (med(u for u, _ in v), med(s for _, s in v))
            for k, v in reps.items()}


def tmfix_tables(run, mains):
    """X against X+TM within the *tm parts (same run, interleaved); "main
    run" is X in the scheduling parts, a reference for run-to-run change."""
    print("| scenario | load | config | p99 without | p99 with TMFIX | change | "
          "p99 in the main run |")
    print("|---|---|---|---|---|---|---|")
    for scenario in ("S1", "S2"):
        part = summarize(read_part(run, scenario + "tm"))
        main = mains.get(scenario, {})
        for (g, load, c), v in sorted(part.items(),
                                      key=lambda kv: (kv[0][1], rank(kv[0][2]))):
            fixed = part.get((g, load, c + TM))
            if c.endswith(TM) or not fixed:
                continue
            ref = main.get((g, load, c))
            p99 = lambda x: (fmt(x["p99"]) if x["ok"] else "OVL") if x else "—"
            print(f"| {scenario} | {load} | {c} | {p99(v)} | {p99(fixed)} | "
                  f"{pct(fixed['p99'], v['p99'])} | {p99(ref)} |")
    print()
    print("| scenario | load | config | server CPU cores (user / kernel) without "
          "| with TMFIX |")
    print("|---|---|---|---|---|")
    for scenario in ("S1", "S2"):
        cpu = server_cpu(run, scenario + "tm")
        for (load, c), v in sorted(cpu.items(), key=lambda kv: (kv[0][0], rank(kv[0][1]))):
            fixed = cpu.get((load, c + TM))
            if c.endswith(TM) or not fixed:
                continue
            print(f"| {scenario} | {load} | {c} | {fmt(v[0])} / {fmt(v[1])} | "
                  f"{fmt(fixed[0])} / {fmt(fixed[1])} |")
    print()
    print("| SPE | load | config | other cluster without | with TMFIX | "
          "tm /req without | with | tm share without | with |")
    print("|---|---|---|---|---|---|---|---|---|")
    for name in ("S1spe_tm", "S2spe_tm"):
        part = spe_part(run, name)
        for (load, c), v in sorted(part.items(), key=lambda kv: (kv[0][0], rank(kv[0][1]))):
            fixed = part.get((load, c + TM))
            if c.endswith(TM) or not fixed:
                continue
            print(f"| {name} | {load} | {c} | {fmt(v['peer_cluster'], 1)} | "
                  f"{fmt(fixed['peer_cluster'], 1)} | {fmt(v['tm_per_req'], 1)} | "
                  f"{fmt(fixed['tm_per_req'], 1)} | {share(v['tm'])} | "
                  f"{share(fixed['tm'])} |")
    print()


def verdict(failed, n):
    return "NO DATA" if n == 0 else ("FAIL" if failed else "PASS")


def compare_to_ca(summaries, config):
    """Points where CA sustains the load and `config` is above CA x
    TOLERANCE or does not sustain it; and how many such points were
    compared."""
    worse = []
    n = 0
    for summary in summaries:
        for (g, load, cfg), v in summary.items():
            ca = summary.get((g, load, "CA"))
            if cfg != config or not ca or not ca["ok"]:
                continue
            n += 1
            if not v["ok"]:
                worse.append(f"{g}@{load} OVL vs {fmt(ca['p99'])}")
            elif v["p99"] > TOLERANCE * ca["p99"]:
                worse.append(f"{g}@{load} {fmt(v['p99'])} vs {fmt(ca['p99'])}")
    return worse, n


def criteria(latency, sweeps_s2, imbalance, spes):
    for c in NEW:
        worse, n = compare_to_ca(latency, c)
        print(f"- {c} p99 <= CA x {TOLERANCE} on every point CA sustains: "
              f"{verdict(worse, n)} ({len(worse)} of {n} above"
              f"{': ' + '; '.join(worse[:6]) if worse else ''})")

        res = []
        for g in COUNTEREXAMPLES:
            v, b0 = sweeps_s2.get((g, 600000, c)), sweeps_s2.get((g, 600000, "B0"))
            if not v or not b0:
                res.append((g, None, "missing"))
                continue
            good = v["ok"] and (not b0["ok"] or v["p99"] <= b0["p99"])
            res.append((g, good, f"{fmt(v['p99']) if v['ok'] else 'OVL'} vs B0 "
                                 f"{fmt(b0['p99']) if b0['ok'] else 'OVL'}"))
        print(f"- {c} <= B0 in the three S2 600k counterexamples: "
              f"{verdict(not all(r[1] for r in res), sum(r[1] is not None for r in res))} ("
              + "; ".join(f"{g} {t}" for g, _, t in res) + ")")

        bad = []
        n = 0
        for name, part in spes.items():
            for (load, cfg), v in part.items():
                ca = part.get((load, "CA"))
                if cfg != c or not ca:
                    continue
                n += 1
                if not (v["peer_cluster"] <= ca["peer_cluster"] and
                        v["queue_per_req"] < ca["queue_per_req"]):
                    bad.append(f"{name}@{load} other cluster "
                               f"{fmt(v['peer_cluster'], 1)} vs "
                               f"{fmt(ca['peer_cluster'], 1)}, queue /req "
                               f"{fmt(v['queue_per_req'], 1)} vs "
                               f"{fmt(ca['queue_per_req'], 1)}")
        print(f"- {c} SPE other-cluster loads <= CA and queue loads per "
              f"request < CA: "
              f"{verdict(bad, n)} ({len(bad)} of {n} not"
              f"{': ' + '; '.join(bad) if bad else ''})")

        worse, n = compare_to_ca(imbalance, c)
        print(f"- {c} imbalance p99 <= CA x {TOLERANCE}: "
              f"{verdict(worse, n)} ({len(worse)} of {n} above"
              f"{': ' + '; '.join(worse) if worse else ''})")
    print()


def main(argv):
    run = pathlib.Path(argv[0])
    print(f"# {run.name}\n")
    mains = {}
    print("## 1. Main scenarios\n")
    for scenario in ("S1", "S2"):
        s = summarize(read_part(run, scenario))
        if s:
            mains[scenario] = s
            latency_tables(scenario, s)
    print("## 2. Sweeps\n")
    sweeps = {}
    for name in ("S1sweep", "S1imb", "S2sweep", "S2imb"):
        s = summarize(read_part(run, name))
        if s:
            sweeps[name] = s
            latency_tables(name, s)
    print("## 3. Queue stats (per 1k = per 1000 tasks taken)\n")
    queue_table([(k, v) for k, v in list(mains.items()) + list(sweeps.items())])
    print("## 4. SPE\n")
    spes = spe_tables(run)
    print("## 5. TMFIX\n")
    tmfix_tables(run, mains)
    print("## 6. Success criteria (design section 3)\n")
    criteria(list(mains.values()) + list(sweeps.values()),
             sweeps.get("S2sweep", {}),
             [sweeps[k] for k in ("S1imb", "S2imb") if k in sweeps],
             spes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
