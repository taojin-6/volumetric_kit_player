#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of the runner, measure, by its --dry-run, which ctest runs
(tests/CMakeLists.txt) with bash and git: each mode prints its commands, in
the order it would run them, and runs and writes none.

DryRun: lab sends a script bash can parse that builds vp_probe and runs it
under uvrun once per timing; iphone builds and installs the app in the Mac's
worktree for runs at the ref's commit and launches it once per timing; mac
builds vp_probe in that worktree on each sibling's clean checkout at its pin,
a worktree of it too, fetches the others, and waits for the Mac to be idle
before the build and each run, whose load goes beside its log; a worktree
already there is checked out instead; a ref the checkout does not know
fetches every sibling.
"""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

MEASURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure")
SIBLINGS = ("core", "recon", "gfx")


def git(cwd, *args):
    """git's output in cwd, with none of the user's or system's config."""
    return subprocess.run(
        ["git", "-c", "user.name=test", "-c", "user.email=test@example.com",
         "-c", "init.defaultBranch=main", *args],
        cwd=cwd, check=True, capture_output=True, text=True,
        env=dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull,
                 GIT_CONFIG_NOSYSTEM="1")).stdout.strip()


class DryRun(unittest.TestCase):
    def setUp(self):
        self.root = os.path.realpath(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.out = os.path.join(self.root, "measurements")
        self.player = os.path.join(self.root, "volumetric_kit_player")
        self.env = dict(
            os.environ,
            MEASURE_CONFIG=os.path.join(self.root, "none.env"),
            MEASURE_OUT=self.out,
            MAC_REPO=self.player,
            MAC_STREAM=os.path.join(self.root, "rafa2.vrst"),
            LAB_HOST="lab.example",
            LAB_ROOT="/lab/measure",
            VP_DEVELOPMENT_TEAM="TEAM",
            MEASURE_DEVICE="device-1",
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
        )

    def repo(self, name, text="1\n"):
        """A repository under the root with one commit; its commit."""
        path = os.path.join(self.root, name)
        os.makedirs(path, exist_ok=True)
        git(path, "init", "-q")
        return self.commit(path, "file.txt", text)

    def commit(self, path, name, text):
        os.makedirs(os.path.dirname(os.path.join(path, name)), exist_ok=True)
        with open(os.path.join(path, name), "w", encoding="utf-8") as file:
            file.write(text)
        git(path, "add", "--", name)
        git(path, "commit", "-q", "-m", name)
        return git(path, "rev-parse", "HEAD")

    def player_at(self, pins):
        """The player's checkout with one commit pinning the siblings."""
        os.makedirs(self.player)
        git(self.player, "init", "-q")
        text = "".join(
            f"FetchContent_Declare(\n  volumetric_kit_{name}\n"
            f"  GIT_REPOSITORY https://example.invalid/{name}.git\n"
            f"  GIT_TAG {pin})\nFetchContent_MakeAvailable("
            f"volumetric_kit_{name})\n" for name, pin in pins.items())
        return self.commit(self.player, "third_party/CMakeLists.txt", text)

    def measure(self, *args):
        done = subprocess.run(["bash", MEASURE, "--dry-run", *args],
                              env=self.env, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertFalse(os.path.exists(self.out), "a dry run wrote logs")
        return done.stdout

    @staticmethod
    def commands(text):
        """Each command printed, as its arguments; a step described in
        parentheses, as its text."""
        return [line[2:] if line.startswith("+ (") else shlex.split(line[2:])
                for line in text.splitlines() if line.startswith("+ ")]

    def test_lab_sends_a_script_that_builds_and_runs_under_uvrun(self):
        text = self.measure("lab", "pr/7", "--passes", "3", "--frames", "9")
        lines = text.splitlines()
        self.assertEqual(lines[0], "+ ssh lab.example bash -s <<'EOF'")
        script = "\n".join(lines[1:lines.index("EOF")])
        parsed = subprocess.run(["bash", "-n"], input=script, text=True,
                                capture_output=True)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        for line in ("passes=3; frames=9", "ref=pr/7",
                     'build_probe uv "$repo" "$root/build"',
                     'run_probe uvtime "$root/build" "$stream" "$timing" '
                     '"$csv"'):
            self.assertIn(line, script)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release -DVP_DESKTOP_APP=OFF",
                      script)
        # bash 5's %q escapes the comma, 3.2's does not.
        self.assertRegex(script, r"for timing in \$\(echo off\\?,all \| tr ,")
        _, mkdir, scp = self.commands(text)
        self.assertEqual(mkdir[:2], ["mkdir", "-p"])
        self.assertRegex(scp[1], r"^lab\.example:/lab/measure/out/"
                         r"\d{8}T\d{6}Z-\*\.csv$")

    def test_a_phone_builds_and_launches_at_the_refs_commit(self):
        sha = self.repo("volumetric_kit_player")
        worktree = os.path.join(self.player, ".worktrees", "measure-run")
        commands = self.commands(
            self.measure("iphone", sha, "--timing", "off"))
        self.assertEqual(commands[0], ["git", "-C", self.player, "worktree",
                                       "add", "--quiet", "--detach", worktree,
                                       sha])
        self.assertEqual(commands[1][2:7],
                         ["cmake", "-S", worktree, "--preset", "ios"])
        launch = commands[-2]
        self.assertEqual(launch[-6:],
                         ["--measure", "rafa2.vrst", "--passes", "2",
                          "--timing", "off"])

    def test_mac_builds_on_the_clean_siblings_at_their_pins(self):
        pins = {name: self.repo(f"volumetric_kit_{name}")
                for name in SIBLINGS}
        # recon at its pin with a tracked change; gfx moved on, a worktree of
        # it still at its pin.
        recon = os.path.join(self.root, "volumetric_kit_recon")
        with open(os.path.join(recon, "file.txt"), "w", encoding="utf-8") as f:
            f.write("changed\n")
        gfx = os.path.join(self.root, "volumetric_kit_gfx")
        at_pin = os.path.join(gfx, ".worktrees", "at-pin")
        git(gfx, "worktree", "add", "-q", "--detach", at_pin, pins["gfx"])
        self.commit(gfx, "file.txt", "2\n")
        sha = self.player_at(pins)
        worktree = os.path.join(self.player, ".worktrees", "measure-run")
        build = os.path.join(worktree, "build-mac")
        commands = self.commands(self.measure(
            "mac", sha, "--passes", "3", "--frames", "9"))
        self.assertEqual(commands[0], ["git", "-C", self.player, "worktree",
                                       "add", "--quiet", "--detach", worktree,
                                       sha])
        self.assertEqual(commands[1], "(wait until this Mac is idle)")
        source = "FETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_"
        self.assertEqual(commands[2], [
            "cmake", "-S", worktree, "-B", build, "-G", "Unix Makefiles",
            "-DCMAKE_BUILD_TYPE=Release", "-DVP_DESKTOP_APP=OFF",
            f"-D{source}CORE={os.path.join(self.root, 'volumetric_kit_core')}",
            f"-U{source}RECON", f"-D{source}GFX={at_pin}"])
        self.assertEqual(commands[3], ["cmake", "--build", build, "--parallel",
                                       "8", "--target", "vp_probe"])
        self.assertEqual(commands[4][:2], ["mkdir", "-p"])
        out = commands[4][2]
        self.assertRegex(out, "^" + re.escape(self.out)
                         + r"/\d{4}-\d\d-\d\d$")
        logs = []
        for at, timing in ((5, "off"), (7, "all")):
            wait, probe = commands[at], commands[at + 1]
            self.assertRegex(probe[-1], "^" + re.escape(out) + r"/\d{8}T\d{6}Z-"
                             + f"{sha[:7]}-mac-{timing}" + r"\.csv$")
            self.assertEqual(wait, "(wait until this Mac is idle, its load "
                             f"into {probe[-1][:-4]}.load)")
            self.assertEqual(probe, [
                os.path.join(build, "tests", "vp_probe"), "--worker", "--file",
                self.env["MAC_STREAM"], "--passes", "3", "--frames", "9",
                "--timing", timing, "--csv", probe[-1]])
            logs.append(probe[-1])
        self.assertEqual(commands[9], "(the logs summarized into "
                         f"{logs[0][:-len('-off.csv')]}.md)")
        self.assertEqual(len(commands), 10)

    def test_mac_checks_out_a_worktree_already_there(self):
        sha = self.player_at({"core": "0" * 40})
        worktree = os.path.join(self.player, ".worktrees", "measure-run")
        os.makedirs(worktree)
        commands = self.commands(self.measure("mac", sha, "--timing", "off"))
        self.assertEqual(commands[0], ["git", "-C", worktree, "checkout",
                                       "--quiet", "--detach", sha])
        source = "FETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_"
        self.assertEqual(commands[2][-3:], [f"-U{source}{name.upper()}"
                                            for name in SIBLINGS])

    def test_mac_fetches_every_sibling_for_a_ref_the_checkout_lacks(self):
        self.repo("volumetric_kit_core")
        self.player_at({})
        commands = self.commands(self.measure("mac", "pr/7"))
        self.assertEqual(commands[0][-1], "pr/7")
        source = "FETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_"
        self.assertEqual(commands[2][-3:], [f"-U{source}{name.upper()}"
                                            for name in SIBLINGS])


if __name__ == "__main__":
    unittest.main()
