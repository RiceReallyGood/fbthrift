"""Shared helpers for the cluster-aware thread pool experiments (suzhou950)."""

import csv
import os
import pathlib
import shlex
import statistics
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
RUN_POINT = HERE / "run_point.sh"

COLUMNS = [
    "stage", "scenario", "config", "placement", "payload", "io", "cpu",
    "conns", "load_pct", "target_qps", "rep", "qps", "p50", "p90", "p99",
    "p999", "max", "fail", "max_burst", "client_cpu_avg", "ctx_switches",
    "migrations", "l3_access", "l3_refill", "local_takes", "stolen_takes",
]

REQUEST_ARGS = "--transport=rocket --compression=none --echo_attachment=true"

# perf event name -> CSV column
PERF_COLUMNS = {
    "context-switches": "ctx_switches",
    "cpu-migrations": "migrations",
    "l3d_cache": "l3_access",
    "ll_cache_rd": "l3_access",
    "l3d_cache_refill": "l3_refill",
    "ll_cache_miss_rd": "l3_refill",
}


def parse_kv(line):
    """'TAG a=1 b=2' -> {'a': '1', 'b': '2'}."""
    return dict(tok.split("=", 1) for tok in line.split()[1:] if "=" in tok)


def parse_perf(path):
    """Parse `perf stat -x,` output into CSV columns."""
    row = {}
    p = pathlib.Path(path)
    if not p.exists():
        return row
    for line in p.read_text().splitlines():
        fields = line.split(",")
        if len(fields) < 3 or not fields[0].strip().replace(".", "").isdigit():
            continue
        event = fields[2].split(":")[0].split("/")[-1] or fields[2]
        col = PERF_COLUMNS.get(event)
        if col:
            row[col] = fields[0]
    return row


def client_args(*, io, cpu, conns, payload, seconds, warmup, qps=None,
                method=None):
    """press_client flags; closed loop when qps is None, else open loop."""
    if qps is None:
        mode = "--mode=close_loop --queue_depth=32"
    else:
        mode = (f"--mode=open_loop --expected_qps={int(qps)} "
                "--open_loop_inflight=100000")
    args = (f"{mode} --io_threads={io} --cpu_threads={cpu} "
            f"--connections={conns} --payload_size={payload} "
            f"--rpc_timeout_ms=10000 --warmup_seconds={warmup} "
            f"--test_seconds={seconds} {REQUEST_ARGS}")
    if method:
        args += f" --method={method}"
    return args


def run_point(out_dir, *, server_cpus, client_cpus, server_args, client_args,
              perf=None, perf_delay=3, perf_secs=10, bin_dir=None,
              dry_run=False, retries=1):
    """Run one point via run_point.sh and return the parsed result row.

    bin_dir selects another benchmark build (e.g. the vanilla-EventBase one).
    """
    cmd = [str(RUN_POINT), "--server-cpus", server_cpus,
           "--client-cpus", client_cpus, "--server-args", server_args,
           "--client-args", client_args, "--out", str(out_dir)]
    if perf:
        cmd += ["--perf", perf, "--perf-delay", str(perf_delay),
                "--perf-secs", str(perf_secs)]
    env = dict(os.environ, BIN=str(bin_dir)) if bin_dir else None
    if dry_run:
        prefix = f"BIN={bin_dir} " if bin_dir else ""
        print(prefix + " ".join(shlex.quote(c) for c in cmd))
        return {}
    for attempt in range(retries + 1):
        p = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if p.returncode == 0:
            break
        if attempt == retries:
            raise RuntimeError(
                f"run_point failed ({p.returncode}): {p.stderr.strip()[-800:]}")
    row = {}
    for line in p.stdout.splitlines():
        if line.startswith("RESULT "):
            row.update(parse_kv(line))
        elif line.startswith("QUEUE_STATS "):
            kv = parse_kv(line)
            row["local_takes"] = kv.get("local", "")
            row["stolen_takes"] = kv.get("stolen", "")
    if perf:
        row.update(parse_perf(pathlib.Path(out_dir) / "perf.txt"))
    return row


def saturation(out_dir, *, reps=3, dry_run=False, **point_kwargs):
    """Median closed-loop QPS over `reps` runs."""
    qps = []
    for rep in range(reps):
        row = run_point(pathlib.Path(out_dir) / f"sat_{rep}", dry_run=dry_run,
                        **point_kwargs)
        if not dry_run:
            qps.append(float(row["qps"]))
    return statistics.median(qps) if qps else 100000.0


def open_loop_knee(out_dir, *, client_args_fn, start=20000, step=20000,
                   max_qps=1000000, p99_limit_us=1000, dry_run=False,
                   **point_kwargs):
    """Highest paced rate (start, start+step, ...) the setup sustains:
    achieved QPS >= 98% of target and p99 <= p99_limit_us. Load levels are
    taken relative to this knee, because closed-loop saturation with deep
    queues (batched reads/writes) overstates open-loop capacity."""
    best = start
    qps = start
    while qps <= max_qps:
        row = run_point(pathlib.Path(out_dir) / f"knee_{qps}",
                        client_args=client_args_fn(qps), dry_run=dry_run,
                        **point_kwargs)
        if dry_run:
            return start
        ok = float(row["qps"]) >= 0.98 * qps and float(row["p99"]) <= p99_limit_us
        print(f"knee probe {qps}: qps={row['qps']} p99={row['p99']} "
              f"{'ok' if ok else 'stop'}", flush=True)
        if not ok:
            break
        best = qps
        qps += step
    return best


def append_csv(path, row):
    path = pathlib.Path(path)
    new = not path.exists()
    with path.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS, extrasaction="ignore",
                           restval="")
        if new:
            w.writeheader()
        w.writerow(row)
