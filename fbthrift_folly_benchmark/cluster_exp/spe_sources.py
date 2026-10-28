#!/usr/bin/env python3
"""Where the server's loads get their data from (Arm SPE on Kunpeng 950).

  sudo perf script -i spe.data --itrace=m \\
      -F comm,tid,event,data_src,weight,ip,sym,dso \\
    | spe_sources.py summarize --period N --secs S --out spe.json
  spe_sources.py table RUN_DIR [RUN_DIR ...]

summarize classifies every sampled load that left its core (missed the core's
L1 and L2) by the HiSilicon HIP12 data source, which perf maps into
perf_mem_data_src, writes the counts, mean latencies and the functions behind
the costly classes to spe.json, and prints one SPE line. table turns the
spe.json files under stage2 run directories (written by run_point.sh --spe)
into loads per request by data source.

perf's --itrace=m gives one llc-access sample for each such load (plus an
llc-miss sample for some of them, which is skipped). --itrace=M would also
give the L1/L2 hits, but loads without a data source packet (TLB-only
accesses) decode there like peer_cpu.

Classes:
  peer_cpu       another core's L2 in the same cluster
  l3             this cluster's L3
  peer_cluster   a cache in another cluster of the same socket
  remote_socket  a cache on the other socket
  local_mem, remote_mem, nc_dev, other
"""

import argparse
import collections
import json
import pathlib
import re
import statistics
import sys

CLASSES = ["peer_cpu", "l3", "peer_cluster", "remote_socket", "local_mem",
           "remote_mem", "nc_dev", "other"]
# cross_cluster is the subset that came from another cluster's cache.
BEYOND_CORE = ["peer_cpu", "l3", "peer_cluster", "remote_socket", "local_mem",
               "remote_mem"]
CROSS_CLUSTER = ["peer_cluster", "remote_socket"]

# include/uapi/linux/perf_event.h
LVL_L2, LVL_L3 = 0x20, 0x40
LVL_LOC_RAM, LVL_REM_RAM1 = 0x80, 0x100
LVL_REM_CCE1, LVL_REM_CCE2, LVL_IO = 0x400, 0x800, 0x1000
SNOOP_HITM = 0x10
SNOOPX_PEER = 0x02


def classify(data_src):
    """perf_mem_data_src value -> (class, hitm)."""
    lvl = (data_src >> 5) & 0x3fff
    snoop = (data_src >> 19) & 0x1f
    snoopx = (data_src >> 38) & 0x3
    hitm = bool(snoop & SNOOP_HITM)
    if lvl & LVL_L2 and snoopx & SNOOPX_PEER:
        return "peer_cpu", hitm
    if lvl & LVL_L3:
        return "l3", hitm
    if lvl & LVL_REM_CCE1:
        return "peer_cluster", hitm
    if lvl & LVL_REM_CCE2:
        return "remote_socket", hitm
    if lvl & LVL_LOC_RAM:
        return "local_mem", hitm
    if lvl & LVL_REM_RAM1:
        return "remote_mem", hitm
    if lvl & LVL_IO:
        return "nc_dev", hitm
    return "other", hitm


def role(comm):
    if comm.startswith(("ThriftIO", "SrvIO")):
        return "io"
    if comm.startswith("SrvCPU") or comm == "press_server":
        return "cpu"  # ThreadManager workers keep the process name
    return "other"


# "comm tid event: data_src |OP ..|LVL ..|SNP ..|TLB ..|LCK ..|BLK .. weight ip
#  sym (dso)"
LINE = re.compile(r"^\s*(?P<comm>\S+)\s+(?P<tid>\d+)\s+(?P<event>[\w-]+):\s+"
                  r"(?P<ds>[0-9a-f]+) \|OP"
                  r".*\|BLK[^|\d]*\s(?P<weight>\d+)\s+(?P<ip>[0-9a-f]+)\s+"
                  r"(?P<sym>.*?)\s+\((?P<dso>[^()]*)\)\s*$")


def parse(line):
    m = LINE.match(line)
    if not m:
        return None
    return dict(comm=m["comm"], event=m["event"], ds=int(m["ds"], 16),
                weight=int(m["weight"]), sym=m["sym"], dso=m["dso"])


def summarize(args):
    n = collections.Counter()
    hitm = collections.Counter()
    lat = collections.Counter()
    by_role = collections.Counter()
    kernel = collections.Counter()
    funcs = {"cross_cluster": collections.Counter(),
             "peer_cpu": collections.Counter()}
    unparsed = 0
    for line in sys.stdin:
        s = parse(line)
        if s is None:
            unparsed += line.strip() != ""
            continue
        if s["event"] != "llc-access":
            continue
        cls, is_hitm = classify(s["ds"])
        n[cls] += 1
        hitm[cls] += is_hitm
        lat[cls] += s["weight"]
        by_role[f"{role(s['comm'])}/{cls}"] += 1
        kernel[cls] += s["dso"] == "[kernel.kallsyms]"
        group = "cross_cluster" if cls in CROSS_CLUSTER else cls
        if group in funcs:
            funcs[group][f"{s['sym']} ({s['dso']})"] += 1
    out = {
        "period": args.period, "secs": args.secs, "samples": sum(n.values()),
        "unparsed": unparsed,
        "classes": {c: {"n": n[c], "hitm": hitm[c], "kernel": kernel[c],
                        "mean_latency": lat[c] / n[c] if n[c] else None}
                    for c in CLASSES},
        "by_role": dict(by_role),
        "top_functions": {g: c.most_common(args.top)
                          for g, c in funcs.items()},
    }
    pathlib.Path(args.out).write_text(json.dumps(out, indent=1) + "\n")
    print(f"SPE period={args.period} secs={args.secs} "
          f"samples={out['samples']} unparsed={unparsed} " +
          " ".join(f"{c}={n[c]}" for c in CLASSES))


def result_qps(client_log):
    for line in pathlib.Path(client_log).read_text(errors="replace").splitlines():
        if line.startswith("RESULT "):
            return float(dict(t.split("=", 1) for t in line.split()[1:]
                              if "=" in t)["qps"])
    return None


def table(args):
    """Median over repetitions of loads per request by data source."""
    for run in args.run_dirs:
        points = collections.defaultdict(list)
        for spe in sorted(pathlib.Path(run).glob("*/*/load*_rep*/spe.json")):
            point = spe.parent
            tag, config = point.parent.parent.name, point.parent.name
            load = point.name.split("_rep")[0][len("load"):]
            qps = result_qps(point / "client.log")
            d = json.loads(spe.read_text())
            if not qps:
                continue
            scale = d["period"] / (qps * d["secs"])  # samples -> per request
            row = {c: d["classes"][c]["n"] * scale for c in CLASSES}
            row["cross_cluster"] = sum(row[c] for c in CROSS_CLUSTER)
            beyond = sum(row[c] for c in BEYOND_CORE)
            row["cross_share"] = row["cross_cluster"] / beyond if beyond else 0
            for c in CROSS_CLUSTER + ["peer_cpu", "l3"]:
                row[f"lat_{c}"] = d["classes"][c]["mean_latency"] or 0
            points[(tag, int(load), config)].append(row)
        print(f"## {run}")
        cols = ["peer_cpu", "l3", "cross_cluster", "local_mem", "remote_mem",
                "cross_share", "lat_peer_cpu", "lat_l3", "lat_peer_cluster"]
        print("| part | load | config | " + " | ".join(cols) + " |")
        print("|" + "---|" * (len(cols) + 3))
        for (tag, load, config), rows in sorted(points.items()):
            med = {c: statistics.median(r[c] for r in rows) for c in cols}
            cells = [f"{med[c]:.2f}" if c == "cross_share" else
                     f"{med[c]:.0f}" if c.startswith("lat_") else
                     f"{med[c]:.1f}" for c in cols]
            print(f"| {tag} | {load} | {config} | " + " | ".join(cells) + " |")
        print()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summarize")
    s.add_argument("--period", type=int, required=True)
    s.add_argument("--secs", type=float, required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--top", type=int, default=30)
    t = sub.add_parser("table")
    t.add_argument("run_dirs", nargs="+")
    args = ap.parse_args()
    summarize(args) if args.cmd == "summarize" else table(args)


if __name__ == "__main__":
    main()
