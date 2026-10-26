#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Run vp_probe with --latency or --save-meshes and check what it prints and
writes (tests/CMakeLists.txt).

    probe_check.py latency DRAWS VP_PROBE ARG...
    probe_check.py save-meshes DIR FRAMES VP_PROBE ARG...

latency: the run exits 0, prints latency figures with samples, the draws'
too when DRAWS is 1, and ends saying no record is left. save-meshes: the run
exits 0 and saves frames 0 to FRAMES - 1 into DIR, each file's PLY header
naming the vertices and triangles its last line states, and that line the
grid's blocks. A frame meshed again, as the next pass's first frame, which
ends the run, may be saved again over its file.
"""

import os
import re
import subprocess
import sys

FIGURES = r" p50 [0-9.]+, p95 [0-9.]+, max [0-9.]+ \(\d+\)"


def run(command):
    """vp_probe's stdout, once it exits 0; exits 1 otherwise."""
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    sys.stdout.write(done.stdout)
    sys.stderr.write(done.stderr)
    if done.returncode != 0:
        print(f"probe_check: vp_probe exited {done.returncode}")
        sys.exit(1)
    return done.stdout.splitlines()


def check_latency(draws, command):
    lines = run(command)
    pattern = "^vp_probe: latency \\(ms\\): mesh to commit" + FIGURES
    if draws:
        pattern += " \\| mesh to first draw" + FIGURES
        pattern += " \\| age at draw" + FIGURES
    pattern += r" \| held \d+, skipped 0$"
    if not any(re.match(pattern, line) for line in lines):
        print(f"probe_check: no line of figures matching {pattern!r}")
        return 1
    ends = [re.match(r"^vp_probe: latency: (\d+) record\(s\) made, (\d+) "
                     r"left$", line) for line in lines]
    ends = [end for end in ends if end]
    if len(ends) != 1 or int(ends[0].group(1)) == 0 or ends[0].group(2) != "0":
        print("probe_check: the run did not end with every record released")
        return 1
    print(f"probe_check: {ends[0].group(1)} records, every one released")
    return 0


def ply_counts(path):
    """The vertex and face counts a PLY file's header names."""
    counts = {}
    with open(path, "rb") as ply:
        for raw in ply:
            line = raw.decode("ascii", "replace").strip()
            if line == "end_header":
                break
            parts = line.split()
            if len(parts) == 3 and parts[0] == "element":
                counts[parts[1]] = int(parts[2])
    return counts.get("vertex"), counts.get("face")


def check_save_meshes(directory, frames, command):
    lines = run(command)
    saved = {}
    for line in lines:
        found = re.match(r"^vp_probe: saved (.+): (\d+) vertices, (\d+) "
                         r"triangles, (\d+) blocks$", line)
        if found:
            saved[found.group(1)] = tuple(int(found.group(i)) for i in (2, 3, 4))
    expected = {os.path.join(directory, f"{i}.ply") for i in range(frames)}
    if set(saved) != expected:
        print(f"probe_check: saved {sorted(saved)}, not {sorted(expected)}")
        return 1
    for path, (vertices, triangles, blocks) in sorted(saved.items()):
        if vertices == 0 or triangles == 0 or blocks == 0:
            print(f"probe_check: {path} is empty or names no block")
            return 1
        if ply_counts(path) != (vertices, triangles):
            print(f"probe_check: {path} holds {ply_counts(path)}, "
                  f"its line says {(vertices, triangles)}")
            return 1
    print(f"probe_check: {len(saved)} meshes saved as their lines say")
    return 0


def main(argv):
    if len(argv) >= 4 and argv[1] == "latency":
        return check_latency(argv[2] == "1", argv[3:])
    if len(argv) >= 5 and argv[1] == "save-meshes":
        return check_save_meshes(argv[2], int(argv[3]), argv[4:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
