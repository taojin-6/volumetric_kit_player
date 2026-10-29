#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of summarize.py, which ctest runs (tests/CMakeLists.txt).

Summarize: on the fixtures in testdata/, a format-1 log (before.csv, M0's),
a format-2 one (after.csv) and a format-3 one (unshared.csv), and the last
as format 4, which names the grid's commit where recon's was, it keeps a
log's passes after the first, takes nearest-rank p50 and p95, takes the GPU
over measured rows alone and says why the rest were not, counts the device
per frame, shows the load in a <log>.load beside a log after its device;
from format 2 it shows the settings, the renderer's table, the dispatches and
barriers, the frames' figures, the latency of a frame with no present apart,
and the rounds; from format 3 whether vertices were shared, and the vertices
per frame; in each it takes a timed frame's device time over its stages but
their breakdowns; it diffs two logs, a format-1 against a format-2 one and a
format-2 against a format-3 one, a figure one lacks shown as "-", keeps a
breakdown that a later frame or the second log alone has under its stage,
and refuses what is not a log of a format it reads.

ProbeLog: each log vp_probe writes of the tests' stream (VP_PROBE_CSV, the
paths joined by os.pathsep), two passes of its six frames with every frame
timed, vertices shared or not and both among the logs, reads as one of
format 4: its header names the commits of core, grid and gfx, which the build
fetched, and says how it ran; each frame has its read, decode, mesh,
submit, wait and finish rows,
the decode's breakdowns where measured, and its frame row: its record's set,
none for the record that names none; three vertices a triangle unshared and
fewer shared; in the kept pass, the second, one batch, one submit and no
staging buffer, with dispatches, barriers and its rounds, its queue idle and
latency in range as ms, and for the worker alone a latency to taken, as it
presents nothing; it skips without VP_PROBE_CSV, which ctest's check fails.
"""

import csv
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize  # noqa: E402

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")
BEFORE = os.path.join(DATA, "before.csv")
AFTER = os.path.join(DATA, "after.csv")
UNSHARED = os.path.join(DATA, "unshared.csv")


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
        self.assertIn("- run: vp_probe --worker --passes 2", text)
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

    def test_a_runs_load_beside_its_log_follows_its_device(self):
        with open(BEFORE, encoding="utf-8") as file:
            path = self.write(file.read())
        load = os.path.splitext(path)[0] + ".load"
        with open(load, "w", encoding="utf-8") as file:
            file.write("load averages 2.34 2.23 2.07; memory free 84%\n")
        self.addCleanup(os.remove, load)
        rows = summarize.summary(summarize.read_log(path)).splitlines()
        at = rows.index("- load: load averages 2.34 2.23 2.07; memory free 84%")
        self.assertTrue(rows[at - 1].startswith("- device: Test GPU;"))
        self.assertNotIn("- load:",
                         summarize.summary(summarize.read_log(BEFORE)))

    def test_two_logs_are_diffed(self):
        text = summarize.summarize([BEFORE, AFTER])
        self.assertEqual(text.count("## fixture.vrst"), 2)
        rows = text.split("## Before -> after (means)")[1].splitlines()
        self.assertIn("| decode | 4.000 -> 2.000 (-50%) | 2.000 -> 0.000 (-100%)"
                      " | 0.700 -> 0.700 (+0%) |", rows)
        self.assertIn("|   ..rans decode | 0.500 -> - | 0.250 -> - "
                      "| 0.250 -> - |", rows)
        self.assertIn("| submits | 5.000 -> 1.000 (-80%) |", rows)
        self.assertIn(f"- before: {BEFORE} (player 1111111"
                      + "a" * 33 + ", vp_probe --worker --passes 2, "
                      "timing all)", rows)
        self.assertNotIn("## Before", summarize.summarize([BEFORE]))

    def test_a_format_2_log_shows_its_settings_render_counts_and_figures(self):
        log = summarize.read_log(AFTER)
        self.assertEqual(log.format, 2)
        self.assertEqual(log.tables["render"].order, ["draw (mesh+UI)"])
        self.assertEqual(log.rounds, {1: 2, 2: 1})
        text = summarize.summary(log)
        rows = text.splitlines()
        self.assertIn("- rings in place, tile_depth 0, labels on", rows)
        self.assertIn("| stage (ms per display frame) | host mean | p50 | p95 "
                      "| wait mean | p50 | p95 | GPU mean | p50 | p95 "
                      "| GPU measured |", rows)
        self.assertIn("| draw (mesh+UI) | 0.500 | 0.500 | 0.500 | 0.000 "
                      "| 0.000 | 0.000 | 1.100 | 1.000 | 1.200 | 2/2 |", rows)
        self.assertIn("| dispatches | 39.00 | 39.00 | 39.00 |", rows)
        self.assertIn("| barriers | 47.00 | 47.00 | 47.00 |", rows)
        # A timed frame's device time is its stages' but their breakdowns'.
        self.assertIn("| device time ms (timed frames) | 1.100 | 1.100 "
                      "| 1.300 |", rows)
        self.assertIn("| queue idle ms | 27.000 | 26.000 | 28.000 |", rows)
        self.assertIn("| queue latency ms | 0.400 | 0.400 | 0.400 |", rows)
        # The latency of the frames presented, and apart of frame 2's, taken
        # with no present.
        self.assertIn("| latency ms (due -> present) | 11.500 | 11.000 "
                      "| 12.000 |", rows)
        self.assertIn("| latency ms (due -> taken; no present) | 11.000 "
                      "| 11.000 | 11.000 |", rows)
        self.assertIn("|   ..taken -> presented ms | 2.000 | 2.000 | 2.000 |",
                      rows)
        self.assertIn("- rounds on the device: 1: 2, 2: 1 frames", rows)
        # Format 1 had no breakdown-free frame time of its own: it is read
        # from the stages, breakdowns aside, the unavailable mesh too.
        before = summarize.read_log(BEFORE)
        self.assertEqual([round(ms, 6) for ms in before.figures["gpu_ms"]],
                         [0.9, 1.1, 0.9])
        self.assertEqual(before.counts["dispatches"], [])

    def test_a_format_1_log_diffs_against_a_format_2_one(self):
        rows = summarize.summarize([BEFORE, AFTER]).split(
            "## Before -> after (means)")[1].splitlines()
        self.assertIn("| dispatches | - -> 39.000 |", rows)
        self.assertIn("| staging buffers | 3.000 -> 0.000 (-100%) |", rows)
        self.assertIn("| device time ms (timed frames) | 0.967 -> 1.100 (+14%) |",
                      rows)
        self.assertIn("| latency ms (due -> present) | - -> 11.500 |", rows)
        self.assertIn("| draw (mesh+UI) | - -> 0.500 | - -> 0.000 "
                      "| - -> 1.100 |", rows)
        # Neither has vertices: no row for them.
        self.assertFalse(any(row.startswith("| vertices") for row in rows))

    def test_a_format_3_log_shows_its_vertex_sharing_and_vertices(self):
        log = summarize.read_log(UNSHARED)
        self.assertEqual(log.format, 3)
        self.assertEqual(log.counts["vertices"], [1500, 1530, 1560])
        self.assertEqual(log.counts["dispatches"], [39, 39, 39])
        rows = summarize.summary(log).splitlines()
        self.assertIn("- rings copied, tile_depth 0, labels on, "
                      "share_vertices off", rows)
        self.assertIn("| vertices | 1530.00 | 1530.00 | 1560.00 |", rows)
        self.assertIn("| latency ms (due -> taken; no present) | 10.000 "
                      "| 10.000 | 11.000 |", rows)
        # Formats 1 and 2 have no vertices to show.
        for path in (BEFORE, AFTER):
            self.assertNotIn("| vertices |",
                             summarize.summary(summarize.read_log(path)))

    def test_a_format_4_log_names_the_grid_where_recon_was(self):
        with open(UNSHARED, encoding="utf-8") as file:
            text = file.read()
        log = summarize.read_log(self.write(
            text.replace("measure 3", "measure 4").replace("# recon:",
                                                           "# grid:")))
        self.assertEqual(log.format, 4)
        self.assertEqual(log.counts["vertices"], [1500, 1530, 1560])
        build = ("- build: player 6666666" + "f" * 33 + " (Release), core "
                 "2222222" + "b" * 33 + ", {} 3333333" + "c" * 33
                 + ", gfx 4444444" + "d" * 33)
        rows = summarize.summary(log).splitlines()
        self.assertIn(build.format("grid"), rows)
        self.assertIn(build.format("recon"),
                      summarize.summary(summarize.read_log(UNSHARED))
                      .splitlines())

    def test_a_format_2_log_diffs_against_a_format_3_one(self):
        rows = summarize.summarize([AFTER, UNSHARED]).split(
            "## Before -> after (means)")[1].splitlines()
        self.assertIn("| vertices | - -> 1530.000 |", rows)
        self.assertIn("| dispatches | 39.000 -> 39.000 (+0%) |", rows)
        self.assertIn(f"- after: {UNSHARED} (player 6666666" + "f" * 33
                      + ", vp_probe --worker, timing all, "
                      "share_vertices off)", rows)
        self.assertIn(f"- before: {AFTER} (player 5555555" + "e" * 33
                      + ", vp_probe --worker, timing all)", rows)

    @staticmethod
    def stage_names(rows):
        """The stage column of a markdown table's rows, header left out."""
        return [row.split(" | ")[0][2:] for row in rows
                if row.startswith("| ") and not row.startswith("| stage")
                and not row.startswith("| per frame")]

    def test_a_breakdown_first_seen_later_stays_under_its_stage(self):
        # A frame with no breakdown first, as when it was not timed.
        with open(BEFORE, encoding="utf-8") as file:
            sampled = file.read().replace(
                "2,0,I,1000,500,  ..rans decode,0.5000,0.2500,0.2000,"
                "measured,,,,\n", "")
        log = summarize.read_log(self.write(sampled))
        self.assertEqual(log.order, ["read", "decode", "  ..rans decode", "mesh"])
        # A log with none diffed against one with it, as timing off then all.
        diffed = summarize.summarize([AFTER, BEFORE])
        # After the heading: its blank line, the logs, the stages, the counts.
        table = diffed.split("## Before -> after (means)")[1].split("\n\n")[2]
        self.assertEqual(self.stage_names(table.splitlines()),
                         ["read", "decode", "  ..rans decode", "mesh"])

    def write(self, text):
        handle, path = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(handle, "w", encoding="utf-8") as file:
            file.write(text)
        self.addCleanup(os.remove, path)
        return path

    def test_what_is_not_a_log_of_this_format_is_refused(self):
        with open(BEFORE, encoding="utf-8") as file:
            good = file.read()
        header_only = good[:good.index("\n1,0,I") + 1]
        with open(AFTER, encoding="utf-8") as file:
            good_2 = file.read()
        with open(UNSHARED, encoding="utf-8") as file:
            good_3 = file.read()
        for bad, why in (
            (good.replace("measure 1", "measure 5"), "format"),
            (good.replace("measure 1", "measure 2"), "lacks rings"),
            (good_2.replace("# labels: on\n", ""), "lacks labels"),
            (good_2.replace("measure 2", "measure 1"), "columns"),
            (good_2.replace("measure 2", "measure 3"), "lacks share_vertices"),
            (good_2.replace("measure 2", "measure 3").replace(
                "# labels: on\n", "# labels: on\n# share_vertices: on\n"),
             "columns"),
            (good_3.replace("measure 3", "measure 4"), "lacks grid"),
            (good_3.replace("8.0000,,1530,501", "8.0000,,1530,five"),
             "set 'five' is not a number"),
            (good_3.replace("8.0000,,1530,501", "8.0000,,,501"),
             "vertices '' is not a number"),
            (good_2.replace(",render,", ",draw,"), "table"),
            (good.replace("# timing: all\n", ""), "lacks timing"),
            (good.replace("# run: vp_probe --worker --passes 2\n", ""),
             "lacks run"),
            (header_only, "no frames"),
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
    # Its frames' sets as the log writes them: set_of's, empty for
    # kNoSetFrame's record, which names none.
    SETS = ["500", "501", "502", "503", "", "505"]

    def setUp(self):
        paths = os.environ.get("VP_PROBE_CSV")
        if not paths:
            self.skipTest("VP_PROBE_CSV names no probe log")
        self.logs = [summarize.read_log(path)
                     for path in paths.split(os.pathsep)]

    def test_the_header_names_what_was_measured(self):
        for log in self.logs:
            with self.subTest(log=log.path):
                self.assertEqual(log.format, 4)
                header = log.header
                for sibling in ("core", "grid", "gfx"):
                    self.assertRegex(header[sibling],
                                     r"^[0-9a-f]{40}(-dirty)?$", sibling)
                self.assertEqual(header["stream"], "sphere.vrst, track 0")
                self.assertRegex(header["run"],
                                 r"^vp_probe( --worker)? --passes 2$")
                self.assertEqual(header["timing"], "all")
                self.assertIn(header["rings"], ("in place", "copied"))
                self.assertEqual(header["tile_depth"], "0")
                self.assertEqual(header["labels"], "on")
                self.assertIn(header["share_vertices"], ("on", "off"))
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

    def test_the_logs_run_vertices_shared_and_not(self):
        self.assertEqual({log.header["share_vertices"] for log in self.logs},
                         {"on", "off"})

    def test_each_frame_has_its_set_and_its_vertices_as_shared(self):
        for log in self.logs:
            with self.subTest(log=log.path):
                with open(log.path, newline="", encoding="utf-8") as file:
                    lines = [line for line in file
                             if not line.startswith("#")]
                frames = [row for row in csv.DictReader(lines)
                          if row["stage"] == ""]
                self.assertEqual(len(frames), 2 * self.FRAMES)
                shared = log.header["share_vertices"] == "on"
                for row in frames:
                    frame = int(row["frame"])
                    self.assertEqual(row["set"], self.SETS[frame],
                                     f"frame {frame}")
                    triangles = int(row["triangles"])
                    vertices = int(row["vertices"])
                    self.assertGreater(triangles, 0, f"frame {frame}")
                    if shared:
                        self.assertLess(vertices, 3 * triangles)
                    else:
                        self.assertEqual(vertices, 3 * triangles,
                                         f"frame {frame}")

    def check_frames(self, log):
        self.assertEqual(log.passes, [1, 2])
        self.assertEqual(log.frames, self.FRAMES)
        for name in ("read", "decode", "mesh", "submit", "wait: GPU",
                     "finish"):
            self.assertEqual(sum(log.stages[name].devices.values()),
                             self.FRAMES, name)
        for name in ("read", "submit", "wait: GPU", "finish"):
            self.assertEqual(set(log.stages[name].devices), {"none"}, name)
        # Every frame timed: decode and mesh measured where the queue can
        # time, each over its breakdowns.
        for name in ("decode", "mesh"):
            devices = set(log.stages[name].devices)
            self.assertTrue(devices <= {"measured", "unavailable"}, devices)
        if "measured" in log.stages["decode"].devices:
            self.assertIn("  ..rans decode", log.order)
            self.assertIn("  ..dispatch", log.order)
            self.assertTrue(all(ms > 0 for ms in log.stages["decode"].gpu))
            self.assertEqual(len(log.figures["gpu_ms"]), self.FRAMES)
        # The second pass's every frame one batch: one submit, at most its
        # fence's wait, no staging buffer; and its dispatches, barriers and
        # rounds on the device.
        counts = log.counts
        for frame in range(self.FRAMES):
            self.assertEqual(counts["submits"][frame], 1, f"frame {frame}")
            self.assertLessEqual(counts["host_waits"][frame], 1)
            self.assertEqual(counts["staging_buffers"][frame], 0)
            self.assertGreater(counts["dispatches"][frame], 0)
            self.assertGreater(counts["barriers"][frame], 0)
        self.assertEqual(sum(log.rounds.values()), self.FRAMES)
        self.assertNotIn(0, log.rounds)
        # The queue idle and latency in ms: the idle under a second, as the
        # stream's longest pts step is 540 ms, and the latency, a submit's,
        # under 100 ms, less than a calibration's deviation before it.
        for idle in log.figures["idle_ms"]:
            self.assertGreaterEqual(idle, 0)
            self.assertLess(idle, 1000)
        for queue in log.figures["queue_ms"]:
            self.assertGreater(queue, -1)
            self.assertLess(queue, 100)
        # The worker alone presents nothing: its latency is to taken.
        if "--worker" in log.header["run"]:
            self.assertEqual(log.figures["latency_ms"], [])
            self.assertEqual(len(log.figures["taken_latency_ms"]),
                             self.FRAMES)


if __name__ == "__main__":
    unittest.main()
