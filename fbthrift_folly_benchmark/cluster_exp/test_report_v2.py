#!/usr/bin/env python3
"""Tests for report_v2.py: python3 -m unittest test_report_v2."""

import contextlib
import csv
import io
import pathlib
import tempfile
import unittest

import exp_common as ec
import report_v2

TAG = "main_p1024_io24_cpu72_c96"


def ca2_stats(local, stolen, assign_local, assign_numa):
    return (f"local={local} stolen={stolen} steal_ratio=0 "
            f"assign_local={assign_local} assign_numa={assign_numa} "
            "assign_far=0 orphan_add=10 orphan_local_dec=2 orphan_take=6 "
            "orphan_miss=2 empty_target=5 handoff=1 grab_take=0 grab_miss=0 "
            "grab_skip=0 wake_local=0,4,6,0,0,0,0,0,0,0,0 "
            "wake_remote=0,0,0,0,0,0,0,0,0,0,0")


def point(config, rep, p99, queue_stats=""):
    return dict(stage="2", scenario="S1", config=config, payload=1024, io=24,
                cpu=72, conns=96, load_pct=200000, target_qps=200000,
                rep=rep, qps="200000.00", p50="100", p99=str(p99),
                fail="0", queue_stats=queue_stats)


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
        self.assertIn("CA2 p99 <= CA x 1.03 on every point both sustain: "
                      "NO DATA", text)


if __name__ == "__main__":
    unittest.main()
