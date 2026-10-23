#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of summarize.py, which ctest runs (tests/CMakeLists.txt).

Summarize: on the fixtures in testdata/, it keeps a log's passes after the
first, takes nearest-rank p50 and p95, takes the GPU over measured rows alone
and says why the rest were not, counts the device per frame, diffs two logs,
and refuses what is not a log of its format.

ProbeLog: each log vp_probe writes of the tests' stream (VP_PROBE_CSV, the
paths joined by os.pathsep), two passes of its six frames with every frame
timed, reads as one: each frame has its read, decode and mesh rows, the
decode's breakdowns where measured, and its device counts; it skips without
VP_PROBE_CSV, which ctest's check fails.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize  # noqa: E402

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")
BEFORE = os.path.join(DATA, "before.csv")
AFTER = os.path.join(DATA, "after.csv")


class Summarize(unittest.TestCase):
    def test_a_log_of_several_passes_leaves_out_its_first(self):
        log = summarize.read_log(BEFORE)
        self.assertEqual(log.passes, [1, 2])
        self.assertEqual(log.kept, [2])
        self.assertEqual(log.frames, 3)
        self.assertEqual(log.order, ["read", "decode", "  ..rans decode", "mesh"])
        self.assertEqual(log.stages["read"].host, [1.0, 2.0, 3.0])
        self.assertEqual(summarize.read_log(AFTER).kept, [1])

    def test_quantiles_are_nearest_rank(self):
        self.assertEqual(summarize.quantile([3.0, 1.0, 2.0], 0.5), 2.0)
        self.assertEqual(summarize.quantile([3.0, 1.0, 2.0], 0.95), 3.0)
        self.assertEqual(summarize.quantile([0.2, 0.3], 0.5), 0.2)
        self.assertEqual(summarize.quantile([0.2, 0.3], 0.95), 0.3)
        self.assertEqual(summarize.quantile(list(range(1, 101)), 0.95), 95)

    def test_the_table_states_host_wait_and_gpu_per_stage(self):
        text = summarize.summary(summarize.read_log(BEFORE))
        self.assertIn("## fixture.vrst, track 0 · timing all · Test GPU", text)
        self.assertIn("passes 2-2 of 2 (pass 1 left out: warm-up) · 3 frames",
                      text)
        self.assertIn("player 1111111aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa (Release)",
                      text)
        rows = text.splitlines()
        self.assertIn("| read | 2.000 | 2.000 | 3.000 | 0.000 | 0.000 | 0.000"
                      " | - | - | - | 0/3 |", rows)
        self.assertIn("| decode | 4.000 | 4.000 | 6.000 | 2.000 | 1.000 | 4.000"
                      " | 0.700 | 0.700 | 0.900 | 3/3 |", rows)

    def test_the_gpu_is_over_measured_rows_and_the_rest_say_why(self):
        rows = summarize.summary(summarize.read_log(BEFORE)).splitlines()
        self.assertIn("|   ..rans decode | 0.500 | 0.500 | 0.500 | 0.250 | 0.250"
                      " | 0.250 | 0.250 | 0.200 | 0.300 | 2/3, 1 n/t |", rows)
        self.assertIn("| mesh | 1.000 | 1.000 | 1.000 | 0.500 | 0.500 | 0.500"
                      " | 0.400 | 0.400 | 0.400 | 2/3, 1 n/a |", rows)

    def test_the_device_counts_are_per_frame(self):
        rows = summarize.summary(summarize.read_log(BEFORE)).splitlines()
        self.assertIn("| submits | 5.00 | 5.00 | 6.00 |", rows)
        self.assertIn("| host waits | 4.00 | 4.00 | 4.00 |", rows)
        self.assertIn("| blocked ms | 2.00 | 2.00 | 3.00 |", rows)
        self.assertIn("| staging buffers | 3.00 | 3.00 | 3.00 |", rows)

    def test_two_logs_are_diffed(self):
        text = summarize.summarize([BEFORE, AFTER])
        self.assertEqual(text.count("## fixture.vrst"), 2)
        rows = text.split("## Before -> after (means)")[1].splitlines()
        self.assertIn("| decode | 4.000 -> 2.000 (-50%) | 2.000 -> 0.000 (-100%)"
                      " | 0.700 -> 0.700 (+0%) |", rows)
        self.assertIn("|   ..rans decode | 0.500 -> - | 0.250 -> - "
                      "| 0.250 -> - |", rows)
        self.assertIn("| submits | 5.000 -> 1.000 (-80%) |", rows)
        self.assertNotIn("## Before", summarize.summarize([BEFORE]))

    def write(self, text):
        handle, path = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(handle, "w", encoding="utf-8") as file:
            file.write(text)
        self.addCleanup(os.remove, path)
        return path

    def test_what_is_not_a_log_of_this_format_is_refused(self):
        with open(BEFORE, encoding="utf-8") as file:
            good = file.read()
        for bad, why in (
            (good.replace("measure 1", "measure 2"), "format"),
            (good.replace("# timing: all\n", ""), "lacks timing"),
            (good.replace("staging_buffers", "staging"), "columns"),
            (good.replace("2,0,I,1000,500,read,1.0000,0.0000,,none",
                          "2,0,I,1000,500,read,1.0000,0.0000,0.1,none"),
             "gpu_ms"),
            (good.replace(",not_timed,", ",untimed,"), "device"),
            (good.replace("2,1,P,600,510,,,,,,5", "2,1,P,600,510,,,,,,five"),
             "not a number"),
        ):
            with self.subTest(why=why):
                with self.assertRaisesRegex(summarize.LogError, why):
                    summarize.read_log(self.write(bad))
        self.assertEqual(summarize.main(["summarize.py", self.write("x\n")]), 1)
        self.assertEqual(summarize.main(["summarize.py"]), 2)


class ProbeLog(unittest.TestCase):
    FRAMES = 6  # the tests' stream's (tests/test_stream.hpp)

    def setUp(self):
        paths = os.environ.get("VP_PROBE_CSV")
        if not paths:
            self.skipTest("VP_PROBE_CSV names no probe log")
        self.logs = [summarize.read_log(path)
                     for path in paths.split(os.pathsep)]

    def test_the_header_names_what_was_measured(self):
        for log in self.logs:
            with self.subTest(log=log.path):
                header = log.header
                self.assertEqual(header["stream"], "sphere.vrst, track 0")
                self.assertEqual(header["timing"], "all")
                self.assertNotEqual(header["build"], "none")
                self.assertRegex(
                    header["device"],
                    r"; vendor 0x[0-9a-f]+; driver .*; timestamps \d+ bits")
                self.assertRegex(header["utc"],
                                 r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")

    def test_two_passes_of_every_frame_with_their_rows_and_counts(self):
        for log in self.logs:
            with self.subTest(log=log.path):
                self.check_frames(log)

    def check_frames(self, log):
        self.assertEqual(log.passes, [1, 2])
        self.assertEqual(log.frames, self.FRAMES)
        for name in ("read", "decode", "mesh"):
            self.assertEqual(sum(log.stages[name].devices.values()),
                             self.FRAMES, name)
        self.assertEqual(set(log.stages["read"].devices), {"none"})
        # Every frame timed: decode and mesh measured where the queue can
        # time, each over its breakdowns.
        for name in ("decode", "mesh"):
            devices = set(log.stages[name].devices)
            self.assertTrue(devices <= {"measured", "unavailable"}, devices)
        if "measured" in log.stages["decode"].devices:
            self.assertIn("  ..rans decode", log.order)
            self.assertIn("  ..dispatch", log.order)
            self.assertTrue(all(ms > 0 for ms in log.stages["decode"].gpu))
        # Each frame's decode and mesh submit, and their fences block.
        self.assertTrue(all(n >= 2 for n in log.counts["submits"]))
        self.assertGreater(sum(log.counts["host_waits"]), 0)


if __name__ == "__main__":
    unittest.main()
