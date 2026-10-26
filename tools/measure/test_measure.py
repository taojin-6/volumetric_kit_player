#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of the runner, measure, which ctest runs with bash and git.
Dry runs print commands without running them. Runner tests use temporary
repositories and command stubs; no build, measurement or device is started.

DryRun: lab sends a script bash can parse that builds vp_probe and runs it
under uvrun once per timing, at the tile depth, labels and vertex sharing
asked for; iphone builds the app on 6 jobs and installs it in the Mac's
worktree for runs at the ref's commit and launches it once per timing, with
the tile depth and labels, and --share-vertices off only when asked; mac
builds vp_probe in that worktree on each sibling's clean checkout at its pin,
a worktree of it too, fetches the others, and waits for the Mac to be idle
before the build and each run, whose load goes beside its log, passing
--share-vertices off only when asked; a worktree already there is checked
out instead; a ref the checkout does not know fetches every sibling.

Runner: failed fetches cannot use cached refs on the Mac or lab, a failed
phone launch stops before polling, and lab logs with spaces in their paths
are copied and summarized in order; a phone's first launch, before the app
has made its measure folder, never asks devicectl for it, so shows no
error, while any other devicectl error shows; and each timing's log is
named for its mode and timing. Mac and lab runs omit the new probe options
at their defaults so older baselines still run, and reject nondefaults an
older probe cannot apply. Phone runs with unshared vertices require the log
to confirm the setting; default sharing still accepts older logs.
"""

import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
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


class MeasureCase(unittest.TestCase):
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


class DryRun(MeasureCase):
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
        text = self.measure("lab", "pr/7", "--passes", "3", "--frames", "9",
                            "--tile-depth", "1", "--labels", "off",
                            "--share-vertices", "off")
        lines = text.splitlines()
        self.assertEqual(lines[0], "+ ssh lab.example bash -s <<'EOF'")
        script = "\n".join(lines[1:lines.index("EOF")])
        parsed = subprocess.run(["bash", "-n"], input=script, text=True,
                                capture_output=True)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertIn('for option in --tile-depth --labels --share-vertices',
                      script)
        for line in ("passes=3; frames=9", "depth=1; labels=off",
                     "sharing=off", "ref=pr/7",
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
        self.assertEqual(commands[2][-4:], ["--", "-allowProvisioningUpdates",
                                            "-jobs", "6"])
        launch = commands[-2]
        self.assertEqual(launch[-10:],
                         ["--measure", "rafa2.vrst", "--passes", "2",
                          "--timing", "off", "--tile-depth", "0", "--labels",
                          "on"])
        self.assertRegex(commands[-1], r"-iphone-off\.csv\)$")
        # Vertices unshared only when asked, as the default is the app's.
        unshared = self.commands(self.measure(
            "iphone", sha, "--timing", "off", "--share-vertices", "off"))
        self.assertEqual(unshared[-2][-4:], ["--labels", "on",
                                             "--share-vertices", "off"])
        shared = self.commands(self.measure(
            "iphone", sha, "--timing", "off", "--share-vertices", "on"))
        self.assertNotIn("--share-vertices", shared[-2])

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

    def test_mac_passes_unshared_vertices_only_when_asked(self):
        sha = self.player_at({"core": "0" * 40})
        for args, passed in (
                ([], []), (["--share-vertices", "on"], []),
                (["--share-vertices", "off"], ["--share-vertices", "off"])):
            with self.subTest(args=args):
                probes = [command for command in self.commands(self.measure(
                    "mac", sha, "--timing", "off", *args))
                          if isinstance(command, list)
                          and command[0].endswith("/vp_probe")]
                self.assertEqual(len(probes), 1, probes)
                at = probes[0].index("--csv")
                self.assertEqual(probes[0][at - len(passed):at], passed)
                self.assertEqual(probes[0][at - len(passed) - 2:
                                           at - len(passed)],
                                 ["--timing", "off"])

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


class Runner(MeasureCase):
    def setUp(self):
        super().setUp()
        self.bin = os.path.join(self.root, "bin")
        os.mkdir(self.bin)
        self.calls = os.path.join(self.root, "calls.jsonl")
        self.env.update(PATH=self.bin + os.pathsep + self.env["PATH"],
                        TEST_CALLS=self.calls,
                        TEST_CSV=os.path.join(os.path.dirname(MEASURE),
                                              "testdata", "before.csv"))
        # A missing stub must fail here, never reach a real build or device.
        for name in ("cmake", "xcrun", "ssh", "scp", "sleep"):
            self.command(name, "sys.exit(77)")
        self.command("uptime", "print('load averages: 0.00 0.00 0.00')")
        self.command("memory_pressure",
                     "print('System-wide memory free percentage: 100%')")

    def command(self, name, body):
        """A logged command stub; bodies run in Python, preserving arguments."""
        path = os.path.join(self.bin, name)
        with open(path, "w", encoding="utf-8") as file:
            file.write(f"#!{sys.executable}\n")
            file.write("import json, os, shutil, subprocess, sys\n")
            file.write("with open(os.environ['TEST_CALLS'], 'a') as log:\n"
                       "    log.write(json.dumps(sys.argv) + '\\n')\n")
            file.write(textwrap.dedent(body) + "\n")
        os.chmod(path, 0o755)

    def commands(self):
        if not os.path.exists(self.calls):
            return []
        with open(self.calls, encoding="utf-8") as file:
            return [[os.path.basename(args[0]), *args[1:]]
                    for args in map(json.loads, file)]

    def measure(self, *args):
        return subprocess.run(["bash", MEASURE, *args], env=self.env,
                              capture_output=True, text=True, timeout=15)

    def test_failed_fetch_rejects_cached_branch_and_pr_on_mac_and_lab(self):
        sha = self.repo("volumetric_kit_player")
        git(self.player, "remote", "add", "origin",
            os.path.join(self.root, "missing-origin"))
        for ref in ("main", "pr/7"):
            git(self.player, "update-ref", f"refs/remotes/origin/{ref}", sha)
        lab = os.path.join(self.root, "lab with spaces")
        os.mkdir(lab)
        os.symlink(self.player, os.path.join(lab, "player"))
        self.env["LAB_ROOT"] = lab
        self.env["LAB_UVRUN"] = os.path.join(self.bin, "uvrun")
        self.command("ssh", "sys.exit(subprocess.run(['bash', '-s']).returncode)")
        self.command("flock", "pass")
        self.command("uvrun", "sys.exit(77)")
        for mode in ("mac", "lab"):
            for ref in ("main", "pr/7"):
                with self.subTest(mode=mode, ref=ref):
                    done = self.measure(mode, ref)
                    self.assertNotEqual(done.returncode, 0)
                    self.assertIn(f"no commit {ref}", done.stderr)
                    self.assertFalse(os.path.exists(os.path.join(
                        self.player, ".worktrees", "measure-run")))
                    self.assertFalse(any(args[0] in ("cmake", "uvrun")
                                         for args in self.commands()))

    def phone_stubs(self, launch_status):
        sha = self.repo("volumetric_kit_player")
        origin = os.path.join(self.root, "origin.git")
        git(self.root, "clone", "--bare", "-q", self.player, origin)
        git(self.player, "remote", "add", "origin", origin)
        self.env["TEST_LAUNCH_STATUS"] = str(launch_status)
        self.env["TEST_LAUNCHED"] = os.path.join(self.root, "launched")
        self.command("cmake", "pass")
        self.command("sleep", "pass")
        self.command("xcrun", """
            args = sys.argv[1:]
            def value(flag):
                return args[args.index(flag) + 1]
            if args[:4] == ['devicectl', 'device', 'process', 'launch']:
                with open(os.environ['TEST_LAUNCHED'], 'w') as marker:
                    marker.write('launched')
                print('mock launch diagnostic', file=sys.stderr)
                sys.exit(int(os.environ['TEST_LAUNCH_STATUS']))
            if args[:4] == ['devicectl', 'device', 'info', 'files']:
                launched = os.path.exists(os.environ['TEST_LAUNCHED'])
                # Locked once, at the first poll after the launch.
                locked = os.environ.get('TEST_LOCKED_ONCE', '')
                if locked and launched and not os.path.exists(locked):
                    with open(locked, 'w') as marker:
                        marker.write('locked')
                    print('ERROR: mock device locked', file=sys.stderr)
                    sys.exit(1)
                if value('--subdirectory') == 'Documents':
                    # The app makes its measure folder at its first launch.
                    names = ['rafa2.vrst'] + (['measure'] if launched else [])
                elif launched:
                    names = ['result.csv']
                else:
                    # As devicectl says of a folder the app has not made.
                    print('ERROR: mock no such folder', file=sys.stderr)
                    sys.exit(1)
                with open(value('--json-output'), 'w') as output:
                    json.dump({'result': names}, output)
            elif args[:4] == ['devicectl', 'device', 'copy', 'from']:
                shutil.copyfile(os.environ['TEST_CSV'], value('--destination'))
            elif args[:4] != ['devicectl', 'device', 'install', 'app']:
                sys.exit('unexpected device command: ' + repr(args))
        """)
        return sha

    def test_failed_phone_launch_returns_its_status_without_polling(self):
        sha = self.phone_stubs(42)
        done = self.measure("iphone", sha, "--timing", "off")
        self.assertEqual(done.returncode, 42, done.stderr)
        self.assertIn("mock launch diagnostic", done.stderr)
        self.assertFalse(any(args[0] == "sleep" for args in self.commands()))
        self.assertFalse(any(args[1:5] == ['devicectl', 'device', 'copy', 'from']
                             for args in self.commands()))

    def test_successful_phone_launch_polls_and_copies_its_log(self):
        sha = self.phone_stubs(0)
        done = self.measure("iphone", sha, "--timing", "off", "--tile-depth",
                            "1", "--labels", "off")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(any(args[0] == "sleep" for args in self.commands()))
        self.assertIn("fixture.vrst", done.stdout)
        # The first launch, before the app made its measure folder, never
        # asks devicectl for it, so shows no error.
        self.assertNotIn("mock no such folder", done.stderr)
        launch = [args for args in self.commands()
                  if args[1:5] == ['devicectl', 'device', 'process', 'launch']]
        self.assertEqual(len(launch), 1)
        self.assertEqual(launch[0][-4:], ["--tile-depth", "1", "--labels",
                                          "off"])
        build = [args for args in self.commands()
                 if args[0] == "cmake" and "--build" in args]
        self.assertEqual(build[0][-2:], ["-jobs", "6"])
        # Named for its mode and timing, as the lab's and the Mac's are.
        logs = [name for _, _, names in os.walk(self.out) for name in names
                if name.endswith(".csv")]
        self.assertEqual(len(logs), 1, logs)
        self.assertRegex(logs[0],
                         r"^\d{8}T\d{6}Z-" + sha[:7] + r"-iphone-off\.csv$")

    def test_any_other_devicectl_error_shows(self):
        sha = self.phone_stubs(0)
        self.env["TEST_LOCKED_ONCE"] = os.path.join(self.root, "locked")
        done = self.measure("iphone", sha, "--timing", "off")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("mock device locked", done.stderr)
        self.assertNotIn("mock no such folder", done.stderr)

    def test_phones_reject_unshared_runs_without_log_confirmation(self):
        sha = self.phone_stubs(0)
        fixtures = os.path.join(os.path.dirname(MEASURE), "testdata")
        shared = os.path.join(self.root, "shared.csv")
        with open(os.path.join(fixtures, "unshared.csv"),
                  encoding="utf-8") as source:
            with open(shared, "w", encoding="utf-8") as target:
                target.write(source.read().replace("# share_vertices: off",
                                                   "# share_vertices: on"))
        for mode in ("iphone", "ipad"):
            for log in (os.path.join(fixtures, "before.csv"),
                        os.path.join(fixtures, "after.csv"), shared):
                with self.subTest(mode=mode, log=log):
                    case = f"{mode}-{os.path.basename(log)}"
                    self.env.update(
                        TEST_CSV=log,
                        TEST_LAUNCHED=os.path.join(self.root, case + ".launch"),
                        MEASURE_OUT=os.path.join(self.out, case))
                    before = len(self.commands())
                    done = self.measure(mode, sha, "--timing", "off",
                                        "--share-vertices", "off")
                    self.assertNotEqual(done.returncode, 0, done.stdout)
                    self.assertIn("does not confirm --share-vertices off",
                                  done.stderr)
                    self.assertNotIn("fixture.vrst", done.stdout)
                    self.assertFalse(any(name.endswith(".md")
                                         for _, _, names in os.walk(
                                             self.env["MEASURE_OUT"])
                                         for name in names))
                    # Keep the rejected raw log for diagnosis but produce no
                    # successful summary.
                    calls = self.commands()[before:]
                    launches = [args for args in calls if args[1:5] ==
                                ['devicectl', 'device', 'process', 'launch']]
                    self.assertEqual(len(launches), 1, launches)
                    self.assertEqual(launches[0][-2:],
                                     ["--share-vertices", "off"])
                    self.assertEqual(sum(args[1:5] ==
                                         ['devicectl', 'device', 'copy', 'from']
                                         for args in calls), 1)

    def test_phones_accept_confirmed_unshared_runs(self):
        sha = self.phone_stubs(0)
        self.env["TEST_CSV"] = os.path.join(os.path.dirname(MEASURE),
                                            "testdata", "unshared.csv")
        for mode in ("iphone", "ipad"):
            with self.subTest(mode=mode):
                self.env["TEST_LAUNCHED"] = os.path.join(self.root, mode)
                before = len(self.commands())
                done = self.measure(mode, sha, "--timing", "off",
                                    "--share-vertices", "off")
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn("share_vertices off", done.stdout)
                launches = [args for args in self.commands()[before:]
                            if args[1:5] ==
                            ['devicectl', 'device', 'process', 'launch']]
                self.assertEqual(len(launches), 1, launches)
                self.assertEqual(launches[0][-2:], ["--share-vertices", "off"])

    def test_phones_omit_default_sharing_for_older_logs(self):
        sha = self.phone_stubs(0)
        for mode in ("iphone", "ipad"):
            for fixture in ("before.csv", "after.csv"):
                for defaults in ([], ["--share-vertices", "on"]):
                    with self.subTest(mode=mode, fixture=fixture,
                                      defaults=defaults):
                        self.env.update(
                            TEST_CSV=os.path.join(os.path.dirname(MEASURE),
                                                  "testdata", fixture),
                            TEST_LAUNCHED=os.path.join(
                                self.root, f"{mode}-{fixture}-{len(defaults)}"))
                        before = len(self.commands())
                        done = self.measure(mode, sha, "--timing", "off",
                                            *defaults)
                        self.assertEqual(done.returncode, 0, done.stderr)
                        self.assertIn("fixture.vrst", done.stdout)
                        launches = [args for args in self.commands()[before:]
                                    if args[1:5] == ['devicectl', 'device',
                                                    'process', 'launch']]
                        self.assertEqual(len(launches), 1, launches)
                        self.assertNotIn("--share-vertices", launches[0])

    def test_lab_copies_whole_log_paths_in_order(self):
        logs = ["/lab/measure runs/out/test-off.csv",
                "/lab/measure runs/out/test-all.csv"]
        self.env["LAB_ROOT"] = "/lab/measure runs"
        self.env["TEST_LOGS"] = json.dumps(logs)
        self.command("ssh", """
            sys.stdin.read()
            print('\\n'.join(json.loads(os.environ['TEST_LOGS'])))
        """)
        self.command("scp", """
            if sys.argv[2] not in ['lab.example:' + path for path in
                                   json.loads(os.environ['TEST_LOGS'])]:
                sys.exit('split remote path: ' + repr(sys.argv[2]))
            shutil.copyfile(os.environ['TEST_CSV'], os.path.join(
                sys.argv[3], os.path.basename(sys.argv[2])))
        """)
        done = self.measure("lab", "main")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual([args[2] for args in self.commands() if args[0] == 'scp'],
                         ['lab.example:' + path for path in logs])
        self.assertLess(done.stdout.index("test-off.csv"),
                        done.stdout.index("test-all.csv"))

    def probe_stubs(self):
        """Local Mac/lab builds of a probe with a configurable option set."""
        sha = self.player_at({})
        origin = os.path.join(self.root, "origin.git")
        git(self.root, "clone", "--bare", "-q", self.player, origin)
        git(self.player, "remote", "add", "origin", origin)
        self.env["LAB_ROOT"] = os.path.join(self.root, "lab with spaces")
        self.env["PLAYER_URL"] = origin
        self.env["LAB_UVRUN"] = os.path.join(self.bin, "uvrun")
        self.env["TEST_PROBE"] = os.path.join(self.bin, "vp_probe")
        self.command("ssh", "sys.exit(subprocess.run(['bash', '-s']).returncode)")
        self.command("flock", "pass")
        self.command("uvrun", """
            args = sys.argv[1:]
            sys.exit(subprocess.run(args[args.index('--') + 1:]).returncode)
        """)
        self.command("cmake", """
            if '--build' in sys.argv:
                build = sys.argv[sys.argv.index('--build') + 1]
                os.makedirs(os.path.join(build, 'tests'), exist_ok=True)
                shutil.copy(os.environ['TEST_PROBE'],
                            os.path.join(build, 'tests', 'vp_probe'))
        """)
        self.command("scp", """
            source = sys.argv[2].split(':', 1)[1]
            shutil.copyfile(source, os.path.join(sys.argv[3],
                                                os.path.basename(source)))
        """)
        self.command("vp_probe", """
            args = sys.argv[1:]
            extra = json.loads(os.environ['TEST_PROBE_FLAGS'])
            known = ['--file', '--passes', '--frames', '--timing', '--csv']
            known += extra
            if not args:
                print('usage: vp_probe --worker ' + ' '.join(known),
                      file=sys.stderr)
                sys.exit(2)
            if args.pop(0) != '--worker':
                sys.exit('expected worker mode')
            while args:
                option, value = args[:2]
                if option not in known:
                    print('unknown option: ' + option, file=sys.stderr)
                    sys.exit(2)
                if option == '--csv':
                    shutil.copyfile(os.environ['TEST_CSV'], value)
                del args[:2]
        """)
        return sha

    def test_default_mac_and_lab_runs_support_old_and_new_probes(self):
        sha = self.probe_stubs()
        for flags in ([], ["--tile-depth", "--labels", "--share-vertices"]):
            self.env["TEST_PROBE_FLAGS"] = json.dumps(flags)
            for mode in ("mac", "lab"):
                for defaults in ([], ["--tile-depth", "0", "--labels", "on",
                                      "--share-vertices", "on"]):
                    with self.subTest(flags=flags, mode=mode, defaults=defaults):
                        before = len(self.commands())
                        done = self.measure(mode, sha, "--timing", "off",
                                            "--passes", "3", "--frames", "9",
                                            *defaults)
                        self.assertEqual(done.returncode, 0, done.stderr)
                        self.assertIn("fixture.vrst", done.stdout)
                        probes = [args for args in self.commands()[before:]
                                  if args[0] == "vp_probe" and len(args) > 1]
                        self.assertEqual(len(probes), 1, probes)
                        self.assertNotIn("--tile-depth", probes[0])
                        self.assertNotIn("--labels", probes[0])
                        self.assertNotIn("--share-vertices", probes[0])
                        self.assertIn("--worker", probes[0])
                        at = probes[0].index("--passes")
                        self.assertEqual(probes[0][at:at + 4],
                                         ["--passes", "3", "--frames", "9"])

    def test_mac_and_lab_reject_nondefaults_the_probe_cannot_apply(self):
        sha = self.probe_stubs()
        self.env["TEST_PROBE_FLAGS"] = "[]"
        for mode in ("mac", "lab"):
            for option, value in (("--tile-depth", "1"), ("--labels", "off"),
                                  ("--share-vertices", "off")):
                with self.subTest(mode=mode, option=option):
                    before = len(self.commands())
                    done = self.measure(mode, sha, "--timing", "off",
                                        option, value)
                    self.assertNotEqual(done.returncode, 0)
                    self.assertIn(f"vp_probe does not support {option}",
                                  done.stderr)
                    self.assertFalse(any(args[0] == "vp_probe" and len(args) > 1
                                         for args in self.commands()[before:]))

    def test_mac_and_lab_pass_supported_nondefaults_to_the_probe(self):
        sha = self.probe_stubs()
        self.env["TEST_PROBE_FLAGS"] = json.dumps(
            ["--tile-depth", "--labels", "--share-vertices"])
        for mode in ("mac", "lab"):
            with self.subTest(mode=mode):
                before = len(self.commands())
                done = self.measure(mode, sha, "--timing", "off",
                                    "--tile-depth", "1", "--labels", "off",
                                    "--share-vertices", "off")
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn("fixture.vrst", done.stdout)
                probes = [args for args in self.commands()[before:]
                          if args[0] == "vp_probe" and len(args) > 1]
                self.assertEqual(len(probes), 1, probes)
                at = probes[0].index("--tile-depth")
                self.assertEqual(probes[0][at:at + 6],
                                 ["--tile-depth", "1", "--labels", "off",
                                  "--share-vertices", "off"])


if __name__ == "__main__":
    unittest.main()
