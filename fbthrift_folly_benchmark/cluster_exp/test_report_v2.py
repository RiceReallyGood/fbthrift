#!/usr/bin/env python3
"""Tests for report_v2.py: python3 -m unittest test_report_v2."""

import contextlib
import csv
import io
import json
import pathlib
import tempfile
import unittest

import exp_common as ec
import report_v2

TAG = "main_p1024_io24_cpu72_c96"


def ca2_stats(local, stolen, assign_local, assign_numa,
              wake_remote="0,0,0,0,0,0,0,0,0,0,0"):
    return (f"local={local} stolen={stolen} steal_ratio=0 "
            f"assign_local={assign_local} assign_numa={assign_numa} "
            "assign_far=0 orphan_add=10 orphan_take=6 "
            "orphan_miss=2 empty_target=5 handoff=1 grab_take=0 grab_miss=0 "
            "grab_skip=0 wake_local=0,4,6,0,0,0,0,0,0,0,0 "
            f"wake_remote={wake_remote}")


def point(config, rep, p99, queue_stats="", load=200000, qps=None):
    return dict(stage="2", scenario="S1", config=config, payload=1024, io=24,
                cpu=72, conns=96, load_pct=load, target_qps=load,
                rep=rep, qps=str(qps if qps is not None else load), p50="100",
                p99=str(p99), fail="0", queue_stats=queue_stats)


def run_report(run):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        report_v2.main([str(run)])
    return out.getvalue()


def write_spe_point(part, config, load, rep, qps, cross_n, symbols):
    point_dir = part / TAG / config / f"load{load}_rep{rep}"
    point_dir.mkdir(parents=True)
    classes = {k: dict(n=0, kernel=0, mean_latency=None) for k in
               ("peer_cpu", "l3", "peer_cluster", "remote_socket",
                "local_mem", "remote_mem", "nc_dev", "other")}
    classes["peer_cluster"]["n"] = cross_n
    (point_dir / "spe.json").write_text(json.dumps(dict(
        period=1000, secs=1.0, samples=cross_n, unparsed=0, classes=classes,
        by_role={}, top_functions=dict(cross_cluster=symbols, peer_cpu=[]))))
    (point_dir / "client.log").write_text(
        f"RESULT qps={qps} p50=90 p90=95 p99=100 p999=110 fail=0\n")


class BucketPercentileTest(unittest.TestCase):
    def test_upper_bound_of_the_bucket(self):
        self.assertEqual(
            report_v2.bucket_percentile([0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0], 0.5),
            2000)

    def test_open_last_bucket(self):
        self.assertEqual(report_v2.bucket_percentile([0] * 10 + [3], 0.99), -1)


class ReportTest(unittest.TestCase):
    def test_main_and_queue_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            part = run / "S1"
            part.mkdir()
            rows = [point("B0", 0, 120), point("B0", 1, 124),
                    point("CA2", 0, 110, ca2_stats(900, 100, 80, 20)),
                    point("CA2", 1, 114, ca2_stats(900, 100, 70, 30))]
            for row in rows:
                ec.append_csv(part / "stage2.csv", row)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                report_v2.main([str(run)])
            text = out.getvalue()
        self.assertIn("| 200000 | B0 | 100 | 122 |", text)
        self.assertIn("| 200000 | CA2 | 100 | 112 | -8.2% |", text)
        # Assignments local: medians of 80% and 70%.
        self.assertRegex(text, r"\| S1 \| 200000 \| CA2 \| 0\.10 \| 75% \|")

    def test_v1_queue_steal_ratio_and_missing_criteria_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            (run / "S1").mkdir()
            for rep in (0, 1):
                row = point("CA", rep, 110)
                row.update(local_takes="800", stolen_takes="200")
                ec.append_csv(run / "S1" / "stage2.csv", row)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                report_v2.main([str(run)])
            text = out.getvalue()
        self.assertIn("| S1 | 200000 | CA | 0.20 |", text)
        self.assertIn("CA2 p99 <= CA x 1.03 on every point CA sustains: "
                      "NO DATA", text)


class CriteriaAndStatsTest(unittest.TestCase):
    def test_ca2_overload_where_ca_sustains_fails_the_criterion(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            (run / "S1").mkdir()
            for rep in (0, 1):
                for row in (point("CA", rep, 100), point("CA2", rep, 100),
                            point("CA", rep, 300, load=630000),
                            point("CA2", rep, 300, load=630000, qps=500000)):
                    ec.append_csv(run / "S1" / "stage2.csv", row)
            text = run_report(run)
        line = next(l for l in text.splitlines()
                    if l.startswith("- CA2 p99 <= CA"))
        self.assertIn("FAIL", line)
        self.assertIn("OVL", line)

    def test_wake_percentiles_pool_the_buckets_of_all_reps(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            (run / "S1").mkdir()
            ec.append_csv(run / "S1" / "stage2.csv", point("CA2", 0, 110, ca2_stats(
                900, 100, 80, 20, wake_remote="0,0,0,0,0,0,0,0,5,0,0")))
            ec.append_csv(run / "S1" / "stage2.csv", point("CA2", 1, 110, ca2_stats(
                900, 100, 80, 20, wake_remote="0,0,0,0,0,0,0,0,0,0,5")))
            text = run_report(run)
        row = next(l for l in text.splitlines()
                   if l.startswith("| S1 | 200000 | CA2 |"))
        # Pooled: half at <=128us, half above 256us.
        self.assertTrue(row.rstrip().endswith("| 128/>256 |"), row)

    def test_spe_queue_loads_per_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            part = run / "S1spe"
            for rep in (0, 1):
                write_spe_point(part, "B0", 200000, rep, 1000, 1000, [
                    ["folly::LifoSemImpl<>::wait() (press_server)", 300]])
                write_spe_point(part, "CA", 200000, rep, 1000, 1000, [
                    ["folly::ClusterAwareBlockingQueue<>::tryDequeue(int) (press_server)", 200]])
                write_spe_point(part, "CA2", 200000, rep, 1000, 400, [
                    ["folly::ClusterAware2BlockingQueue<>::tryClusters(int) (press_server)", 20]])
            text = run_report(run)
        rows = {l.split("|")[2].strip(): l for l in text.splitlines()
                if l.startswith("| 200000 |") and "spe" not in l}
        # period 1000 / (qps 1000 * 1 s): one sample = one load per request.
        self.assertIn("| 300.0 |", rows["B0"])  # LifoSem counts as queue
        self.assertIn("| 200.0 |", rows["CA"])
        self.assertIn("| 20.0 |", rows["CA2"])
        line = next(l for l in text.splitlines()
                    if l.startswith("- CA2 SPE"))
        self.assertIn("PASS", line)


class TmfixTest(unittest.TestCase):
    def test_pairs_within_the_tm_part_and_main_run_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            (run / "S1").mkdir()
            (run / "S1tm").mkdir()
            for rep in (0, 1):
                ec.append_csv(run / "S1" / "stage2.csv", point("CA2", rep, 110))
                ec.append_csv(run / "S1tm" / "stage2.csv", point("CA2", rep, 112))
                ec.append_csv(run / "S1tm" / "stage2.csv",
                              point("CA2+TM", rep, 100))
            text = run_report(run)
        self.assertIn("| S1 | 200000 | CA2 | 112 | 100 | -10.7% | 110 |", text)


if __name__ == "__main__":
    unittest.main()
