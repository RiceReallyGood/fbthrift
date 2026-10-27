#!/usr/bin/env python3
"""Stage 2: compare thread-pool configurations inside emulated containers.

S1 = 96T on one NUMA node (CPUs 0-95), S2 = 192T on two NUMA nodes of one
socket (CPUs 0-191); the client runs on the other socket (CPUs 192-383).

Configs (I/C = IO/CPU threads):
  B0  ThriftServer defaults (ThreadManager), unpinned          - baseline
  B1  lean executor, one global queue, unpinned
  B2  IO/CPU threads pinned round-robin to clusters, global queue
  K1  B0 with kernel.sched_cluster=1
  CA  B2 + ClusterAwareBlockingQueue
  EB  testEb (thread='eb'): handler on the IO thread, I+C IO threads

Parts: main (all configs, 1KB), payload (B0/CA at 64B and 16KB),
ratio (B0/CA at IO:CPU 1:7 and 1:1), imbalance (B0/CA with 4 connections).
Every part runs at 30/50/70/90% (imbalance: 30/50/70%) of B0's open-loop
knee measured for that part, the same absolute QPS for every config. With
--abs-loads the listed absolute QPS values are used instead (load_pct then
holds the QPS), which shows where each config saturates.

B1/B2/CA use folly::ClusterAwareExecutor (B1/B2 with one global queue) behind
DirectThreadManager.
"""

import argparse
import datetime
import pathlib
import subprocess

import exp_common as ec

SCENARIOS = {
    # server cpus, main/1:7/1:1 (io, cpu), client io/cpu threads
    "S1": dict(cpus="0-95", main=(24, 72), r17=(12, 84), r11=(48, 48),
               client=(48, 32)),
    "S2": dict(cpus="0-191", main=(48, 144), r17=(24, 168), r11=(96, 96),
               client=(64, 48)),
}
CLIENT_CPUS = "192-383"
LOADS = [30, 50, 70, 90]
IMBALANCE_LOADS = [30, 50, 70]
PERF_EVENTS = "context-switches,cpu-migrations,l3d_cache,l3d_cache_refill"
ALL_CONFIGS = ["B0", "B1", "B2", "K1", "CA", "EB"]


def server_args(config, io, cpu):
    if config == "EB":
        return f"--pool_mode=tm --num_io_threads={io + cpu} --num_cpu_threads=1"
    mode = {"B0": "tm", "K1": "tm", "B1": "cpu", "B2": "cpu_pinned",
            "CA": "cluster"}[config]
    return f"--pool_mode={mode} --num_io_threads={io} --num_cpu_threads={cpu}"


def set_sched_cluster(value, dry_run):
    cmd = ["sudo", "-n", "sysctl", "-q", "-w", f"kernel.sched_cluster={value}"]
    if dry_run:
        print(" ".join(cmd))
        return
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=SCENARIOS, required=True)
    ap.add_argument("--parts", default="main,payload,ratio,imbalance")
    ap.add_argument("--only-main", action="store_true")
    ap.add_argument("--configs", default=None,
                    help="comma-separated subset of configs to run")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--knee-step", type=int, default=50000)
    ap.add_argument("--abs-loads", default=None,
                    help="comma-separated absolute QPS list (skips knee)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    sc = SCENARIOS[args.scenario]
    parts = ["main"] if args.only_main else args.parts.split(",")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = pathlib.Path(args.out or pathlib.Path.home() /
                       f"cluster_aware/results/stage2/{args.scenario}_{stamp}")
    out.mkdir(parents=True, exist_ok=True)
    csv_path = out / "stage2.csv"
    (out / "perf_events.txt").write_text(PERF_EVENTS + "\n")
    cio, ccpu = sc["client"]

    # (part, configs, payload, (io, cpu), conns or None, loads)
    groups = []
    if "main" in parts:
        groups.append(("main", ALL_CONFIGS, 1024, sc["main"], None, LOADS))
    if "payload" in parts:
        for payload in (64, 16384):
            groups.append(("payload", ["B0", "CA"], payload, sc["main"], None,
                           LOADS))
    if "ratio" in parts:
        for key in ("r17", "r11"):
            groups.append(("ratio", ["B0", "CA"], 1024, sc[key], None, LOADS))
    if "imbalance" in parts:
        groups.append(("imbalance", ["B0", "CA"], 1024, sc["main"], 4,
                       IMBALANCE_LOADS))

    for part, configs, payload, (io, cpu), conns, loads in groups:
        if args.configs:
            configs = [c for c in configs if c in args.configs.split(",")]
        conns = conns or 4 * io
        tag = f"{part}_p{payload}_io{io}_cpu{cpu}_c{conns}"

        def client(qps, seconds=args.seconds, warmup=args.warmup,
                   method=None):
            return ec.client_args(io=cio, cpu=ccpu, conns=conns,
                                  payload=payload, seconds=seconds,
                                  warmup=warmup, qps=qps, method=method)

        base = dict(server_cpus=sc["cpus"], client_cpus=CLIENT_CPUS,
                    server_args=server_args("B0", io, cpu))
        if args.abs_loads:
            abs_loads = [int(x) for x in args.abs_loads.split(",")]
            run_loads(args, out, csv_path, tag, part, configs, payload, io,
                      cpu, conns, sc, client,
                      [(q, q) for q in abs_loads])
            continue
        sat = ec.saturation(out / tag / "saturation", reps=1,
                            dry_run=args.dry_run, client_args=client(None),
                            **base)
        knee = ec.open_loop_knee(
            out / tag / "knee", start=args.knee_step, step=args.knee_step,
            max_qps=sat, dry_run=args.dry_run,
            client_args_fn=lambda q: client(q, seconds=8, warmup=2), **base)
        print(f"[{tag}] B0 closed-loop sat={sat:.0f} knee={knee}", flush=True)
        with (out / "reference_qps.txt").open("a") as f:
            f.write(f"{tag} closed_loop_saturation={sat:.0f} "
                    f"open_loop_knee={knee}\n")

        run_loads(args, out, csv_path, tag, part, configs, payload, io, cpu,
                  conns, sc, client,
                  [(load, knee * load / 100.0) for load in loads])
    print(f"results: {csv_path}")


def run_loads(args, out, csv_path, tag, part, configs, payload, io, cpu, conns,
              sc, client, loads):
    """loads: [(load_pct label, target QPS)]."""
    if True:
        for rep in range(args.reps):
            for load, target in loads:
                for config in configs:
                    point_dir = out / tag / config / f"load{load}_rep{rep}"
                    kwargs = dict(
                        server_cpus=sc["cpus"], client_cpus=CLIENT_CPUS,
                        server_args=server_args(config, io, cpu),
                        client_args=client(
                            target, method="testEb" if config == "EB" else None),
                        perf=PERF_EVENTS, perf_delay=args.warmup + 1,
                        perf_secs=max(1, args.seconds - 4),
                        dry_run=args.dry_run)
                    if config == "K1":
                        set_sched_cluster(1, args.dry_run)
                        try:
                            row = ec.run_point(point_dir, **kwargs)
                        finally:
                            set_sched_cluster(0, args.dry_run)
                    else:
                        row = ec.run_point(point_dir, **kwargs)
                    if args.dry_run:
                        continue
                    row.update(stage="2", scenario=args.scenario, config=config,
                               placement="", payload=payload, io=io, cpu=cpu,
                               conns=conns, load_pct=load,
                               target_qps=int(target), rep=rep)
                    ec.append_csv(csv_path, row)
                    print(f"[{tag}] rep={rep} load={load}% {config}: "
                          f"qps={row['qps']} p50={row['p50']} p99={row['p99']} "
                          f"p999={row['p999']} fail={row['fail']}", flush=True)


if __name__ == "__main__":
    main()
