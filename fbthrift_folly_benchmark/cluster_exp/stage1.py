#!/usr/bin/env python3
"""Stage 1: P99 cost of the IO->CPU handoff by thread placement only.

4 IO threads pinned to CPUs 0-3 (cluster 0); 12 CPU threads pinned to 12
CPUs of the same cluster (P1), another cluster on the same NUMA node (P2),
the other NUMA node of the same socket (P3) or the other socket (P4).
All placements run at the same absolute QPS: 30/50/70/90% of P1's open-loop
knee (highest paced rate with >=98% achieved and p99 <= 1 ms).
With --with-vanilla, P1 and P2 are also run on the build without the folly
EventBase loop-callback wakeup fix (labelled P1-vanilla / P2-vanilla).
"""

import argparse
import datetime
import pathlib

import exp_common as ec

IO_CPUS = "0-3"
PLACEMENTS = {"P1": "4-15", "P2": "20-31", "P3": "100-111", "P4": "196-207"}
CLIENT_CPUS = "288-383"
LOADS = [30, 50, 70, 90]
VANILLA_BIN = pathlib.Path.home() / "cluster_aware/build/bench-vanilla"


def server(cpu_cpus):
    cpus = f"{IO_CPUS},{cpu_cpus}"
    args = (f"--pool_mode=manual --num_io_threads=4 --num_cpu_threads=12 "
            f"--io_cpus={IO_CPUS} --cpu_cpus={cpu_cpus}")
    return cpus, args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--payload", type=int, default=1024)
    ap.add_argument("--out", default=None)
    ap.add_argument("--with-vanilla", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = pathlib.Path(args.out or
                       pathlib.Path.home() / "cluster_aware/results/stage1" / stamp)
    out.mkdir(parents=True, exist_ok=True)
    csv_path = out / "stage1.csv"

    def client(qps=None):
        return ec.client_args(io=16, cpu=16, conns=16, payload=args.payload,
                              seconds=args.seconds, warmup=args.warmup, qps=qps)

    s_cpus, s_args = server(PLACEMENTS["P1"])
    sat = ec.saturation(out / "saturation", reps=args.reps,
                        dry_run=args.dry_run, server_cpus=s_cpus,
                        client_cpus=CLIENT_CPUS, server_args=s_args,
                        client_args=client())
    print(f"P1 closed-loop saturation QPS = {sat:.0f}", flush=True)

    def probe(qps):
        return ec.client_args(io=16, cpu=16, conns=16, payload=args.payload,
                              seconds=8, warmup=2, qps=qps)

    knee = ec.open_loop_knee(out / "knee", client_args_fn=probe,
                             max_qps=sat, dry_run=args.dry_run,
                             server_cpus=s_cpus, client_cpus=CLIENT_CPUS,
                             server_args=s_args)
    print(f"P1 open-loop knee QPS = {knee:.0f}", flush=True)
    (out / "reference_qps.txt").write_text(
        f"closed_loop_saturation={sat:.0f}\nopen_loop_knee={knee:.0f}\n")

    runs = [(name, cpu_cpus, None) for name, cpu_cpus in PLACEMENTS.items()]
    if args.with_vanilla:
        runs += [(f"{name}-vanilla", PLACEMENTS[name], VANILLA_BIN)
                 for name in ("P1", "P2")]

    # Interleave placements inside each (rep, load) to spread machine drift.
    for rep in range(args.reps):
        for load in LOADS:
            target = knee * load / 100.0
            for name, cpu_cpus, bin_dir in runs:
                s_cpus, s_args = server(cpu_cpus)
                row = ec.run_point(out / name / f"load{load}_rep{rep}",
                                   server_cpus=s_cpus, client_cpus=CLIENT_CPUS,
                                   server_args=s_args,
                                   client_args=client(target),
                                   bin_dir=bin_dir, dry_run=args.dry_run)
                if args.dry_run:
                    continue
                row.update(stage="1", scenario="stage1", config="manual",
                           placement=name, payload=args.payload, io=4, cpu=12,
                           conns=16, load_pct=load, target_qps=int(target),
                           rep=rep)
                ec.append_csv(csv_path, row)
                print(f"rep={rep} load={load}% {name}: qps={row['qps']} "
                      f"p50={row['p50']} p99={row['p99']} fail={row['fail']}",
                      flush=True)
    print(f"results: {csv_path}")


if __name__ == "__main__":
    main()
