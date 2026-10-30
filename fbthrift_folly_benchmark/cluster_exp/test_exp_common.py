#!/usr/bin/env python3
"""Tests for the experiment helpers: python3 -m unittest test_exp_common."""

import unittest

import exp_common as ec
import stage2


class ParsePointStdoutTest(unittest.TestCase):
    def test_result_and_queue_stats(self):
        row = ec.parse_point_stdout(
            "RESULT qps=1 p99=2\n"
            "QUEUE_STATS local=3 stolen=4 assign_local=5 wake_local=1,2\n")
        self.assertEqual(row["qps"], "1")
        self.assertEqual(row["p99"], "2")
        self.assertEqual(row["local_takes"], "3")
        self.assertEqual(row["stolen_takes"], "4")
        self.assertEqual(row["queue_stats"],
                         "local=3 stolen=4 assign_local=5 wake_local=1,2")

    def test_queue_stats_is_a_csv_column(self):
        self.assertIn("queue_stats", ec.COLUMNS)


class ServerArgsTest(unittest.TestCase):
    def test_new_configs(self):
        self.assertEqual(
            stage2.server_args("CA2-G", 24, 72),
            "--pool_mode=cluster2_grab --num_io_threads=24 --num_cpu_threads=72")
        self.assertEqual(
            stage2.server_args("CA2", 24, 72),
            "--pool_mode=cluster2 --num_io_threads=24 --num_cpu_threads=72")
        self.assertEqual(
            stage2.server_args("ST", 24, 72),
            "--pool_mode=striped --num_io_threads=24 --num_cpu_threads=72")
        self.assertEqual(
            stage2.server_args("ST-P", 24, 72),
            "--pool_mode=striped_pinned --num_io_threads=24 "
            "--num_cpu_threads=72")

    def test_eb_unchanged(self):
        self.assertEqual(stage2.server_args("EB", 24, 72),
                         "--pool_mode=tm --num_io_threads=96 --num_cpu_threads=1")

    def test_sweeps_run_every_scheduler(self):
        self.assertEqual(stage2.SWEEP_CONFIGS,
                         ["B0", "CA", "CA2", "CA2-G", "ST", "ST-P"])
        for config in stage2.SWEEP_CONFIGS + ["EB"]:
            self.assertIn(config, stage2.ALL_CONFIGS)


class TmfixPairTest(unittest.TestCase):
    def test_tm_configs_run_the_base_config_on_the_tmfix_build(self):
        self.assertEqual(stage2.server_args("CA2+TM", 24, 72),
                         stage2.server_args("CA2", 24, 72))
        self.assertEqual(stage2.bin_dir_for("CA2+TM", "/main", "/tm"), "/tm")
        self.assertEqual(stage2.bin_dir_for("CA2", "/main", "/tm"), "/main")
        for base in ("B0", "CA", "CA2", "CA2-G"):
            self.assertIn(base + "+TM", stage2.ALL_CONFIGS)

    def test_tm_configs_need_the_tmfix_build(self):
        with self.assertRaises(ValueError):
            stage2.bin_dir_for("B0+TM", None, None)


if __name__ == "__main__":
    unittest.main()
