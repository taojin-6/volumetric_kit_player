#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Summarize the player's measure logs as markdown.

usage: summarize.py LOG.csv [LOG.csv ...]

A log is the player's (src/volumetric_kit/player/measure_log.hpp, format 1),
as vp_probe and the iOS app's measure launch write it. Per log, under what it
measured, and after its device the machine's load before the run if a
<log>.load beside it says (the runner's mac mode writes one): per stage, the
mean, p50 and p95 of its host (busy), wait and GPU
milliseconds, and how many of its frames had the GPU measured; then the
worker's device counts per frame. Given two logs, a before/after diff of the
first and the second follows. A log of several passes leaves out its first,
which warms up. p50 and p95 are nearest-rank. A stage first seen in a later
frame, or only in the second log, goes after the row it follows there, so a
breakdown stays under its stage, as the player's stats place it
(PlaybackStats::stage_means).
"""

from __future__ import annotations

import csv
import math
import os
import sys
from dataclasses import dataclass, field

FORMAT = "volumetric_kit_player measure 1"
HEADER_KEYS = (
    "format",
    "player",
    "core",
    "recon",
    "gfx",
    "build",
    "device",
    "stream",
    "run",
    "timing",
    "utc",
)
COLUMNS = [
    "pass",
    "frame",
    "type",
    "bytes",
    "triangles",
    "stage",
    "host_ms",
    "wait_ms",
    "gpu_ms",
    "device",
    "submits",
    "host_waits",
    "blocked_ns",
    "staging_buffers",
]
DEVICES = ("none", "measured", "not_timed", "unavailable")
COUNTS = ("submits", "host_waits", "blocked_ns", "staging_buffers")


class LogError(ValueError):
    """A file that is not a measure log of this format."""


@dataclass
class Stage:
    """A stage's figures over the frames kept."""

    host: list = field(default_factory=list)
    wait: list = field(default_factory=list)
    gpu: list = field(default_factory=list)
    devices: dict = field(default_factory=dict)  # state -> frames


@dataclass
class Log:
    """A log's header and its figures over the passes kept."""

    path: str
    header: dict
    passes: list  # every pass in the log
    kept: list  # the passes the figures are over
    frames: int = 0
    order: list = field(default_factory=list)  # stage names (place)
    stages: dict = field(default_factory=dict)
    counts: dict = field(default_factory=lambda: {key: [] for key in COUNTS})
    load: str = ""  # the machine's load before the run (read_load)


def read_load(path):
    """The load in the <log>.load beside the log at path, or empty."""
    load = os.path.splitext(path)[0] + ".load"
    try:
        with open(load, encoding="utf-8") as file:
            return file.read().strip()
    except FileNotFoundError:
        return ""


def quantile(values, q):
    """The nearest-rank q-quantile of values, which are not empty."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def mean(values):
    return sum(values) / len(values)


def place(order, name, at):
    """Place a frame's stage `name` in `order`: if new, at index `at`, just
    after the stage before it in its frame. Returns the index after it."""
    if name in order:
        return order.index(name) + 1
    order.insert(at, name)
    return at + 1


def number(row, key, path, kind=float):
    try:
        return kind(row[key])
    except ValueError as error:
        raise LogError(f"{path}: {key} {row[key]!r} is not a number") from error


def read_log(path):
    """The log at path, or LogError naming what is wrong with it."""
    with open(path, newline="", encoding="utf-8") as file:
        lines = file.read().splitlines()
    header = {}
    at = 0
    while at < len(lines) and lines[at].startswith("#"):
        key, colon, value = lines[at][1:].strip().partition(":")
        if not colon:
            raise LogError(f"{path}: header line {at + 1} is not '# key: value'")
        header[key.strip()] = value.strip()
        at += 1
    missing = [key for key in HEADER_KEYS if key not in header]
    if missing:
        raise LogError(f"{path}: the header lacks {', '.join(missing)}")
    if header["format"] != FORMAT:
        raise LogError(f"{path}: format {header['format']!r}, not {FORMAT!r}")
    reader = csv.DictReader(lines[at:])
    if reader.fieldnames != COLUMNS:
        raise LogError(f"{path}: columns {reader.fieldnames}, not {COLUMNS}")
    rows = list(reader)
    if not rows:
        raise LogError(f"{path}: no frames")
    passes = sorted({number(row, "pass", path, int) for row in rows})
    log = Log(path, header, passes, passes[1:] if len(passes) > 1 else passes,
              load=read_load(path))
    at = 0  # where the frame's next new stage goes in log.order
    for row in rows:
        if number(row, "pass", path, int) not in log.kept:
            continue
        if row["stage"] == "":  # a frame's counts end it
            log.frames += 1
            for key in COUNTS:
                log.counts[key].append(number(row, key, path, int))
            at = 0
            continue
        device = row["device"]
        if device not in DEVICES:
            raise LogError(f"{path}: device {device!r} is none of {DEVICES}")
        if (device == "measured") != (row["gpu_ms"] != ""):
            raise LogError(
                f"{path}: {row['stage']!r} has gpu_ms {row['gpu_ms']!r} "
                f"with device {device}"
            )
        name = row["stage"]
        at = place(log.order, name, at)
        stage = log.stages.setdefault(name, Stage())
        stage.host.append(number(row, "host_ms", path))
        stage.wait.append(number(row, "wait_ms", path))
        if device == "measured":
            stage.gpu.append(number(row, "gpu_ms", path))
        stage.devices[device] = stage.devices.get(device, 0) + 1
    return log


def figures(values, digits=3):
    """Mean, p50 and p95 cells; dashes for none."""
    if not values:
        return ["-", "-", "-"]
    return [f"{value:.{digits}f}" for value in
            (mean(values), quantile(values, 0.5), quantile(values, 0.95))]


def timed_cell(stage):
    """How many of the stage's frames had the GPU measured, and why not."""
    frames = sum(stage.devices.values())
    cell = f"{stage.devices.get('measured', 0)}/{frames}"
    for state, short in (("not_timed", "n/t"), ("unavailable", "n/a")):
        if stage.devices.get(state):
            cell += f", {stage.devices[state]} {short}"
    return cell


def passes_text(log):
    if len(log.passes) == 1:
        return f"pass {log.passes[0]}"
    return (f"passes {log.kept[0]}-{log.kept[-1]} of {len(log.passes)} "
            f"(pass {log.passes[0]} left out: warm-up)")


def summary(log):
    """The markdown for one log."""
    h = log.header
    out = [
        f"## {h['stream']} · timing {h['timing']} · {h['device'].split(';')[0]}",
        "",
        f"- device: {h['device']}",
        *([f"- load: {log.load}"] if log.load else []),
        f"- run: {h['run']}",
        f"- build: player {h['player']} ({h['build']}), core {h['core']}, "
        f"recon {h['recon']}, gfx {h['gfx']}",
        f"- {h['utc']} · {passes_text(log)} · {log.frames} frames · {log.path}",
        "",
        "| stage (ms per stream frame) | host mean | p50 | p95 "
        "| wait mean | p50 | p95 | GPU mean | p50 | p95 | GPU measured |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for name in log.order:
        stage = log.stages[name]
        cells = (figures(stage.host) + figures(stage.wait)
                 + figures(stage.gpu) + [timed_cell(stage)])
        out.append(f"| {name} | " + " | ".join(cells) + " |")
    out += [
        "",
        "| per frame | mean | p50 | p95 |",
        "|---|---:|---:|---:|",
    ]
    for key in COUNTS:
        label, values = count(log, key)
        out.append(f"| {label} | " + " | ".join(figures(values, 2)) + " |")
    return "\n".join(out)


def count(log, key):
    """A device count's label and its values per frame, blocked time in ms."""
    if key == "blocked_ns":
        return "blocked ms", [value * 1e-6 for value in log.counts[key]]
    return key.replace("_", " "), log.counts[key]


def change(before, after):
    """'before -> after (+x%)' for two means, or what is missing."""
    if not before and not after:
        return "-"
    if not before or not after:
        b = f"{mean(before):.3f}" if before else "-"
        a = f"{mean(after):.3f}" if after else "-"
        return f"{b} -> {a}"
    b, a = mean(before), mean(after)
    percent = f" ({(a - b) / b * 100:+.0f}%)" if b != 0 else ""
    return f"{b:.3f} -> {a:.3f}{percent}"


def diff(before, after):
    """The markdown comparing two logs' means, stage by stage."""
    names = list(before.order)
    at = 0
    for name in after.order:
        at = place(names, name, at)
    out = [
        "## Before -> after (means)",
        "",
        f"- before: {before.path} (player {before.header['player']}, "
        f"{before.header['run']}, timing {before.header['timing']})",
        f"- after: {after.path} (player {after.header['player']}, "
        f"{after.header['run']}, timing {after.header['timing']})",
        "",
        "| stage (ms per stream frame) | host | wait | GPU |",
        "|---|---|---|---|",
    ]
    empty = Stage()
    for name in names:
        b = before.stages.get(name, empty)
        a = after.stages.get(name, empty)
        out.append(f"| {name} | {change(b.host, a.host)} | "
                   f"{change(b.wait, a.wait)} | {change(b.gpu, a.gpu)} |")
    out += ["", "| per frame | before -> after |", "|---|---|"]
    for key in COUNTS:
        label, b = count(before, key)
        out.append(f"| {label} | {change(b, count(after, key)[1])} |")
    return "\n".join(out)


def summarize(paths):
    """The markdown for the logs at paths."""
    logs = [read_log(path) for path in paths]
    parts = [summary(log) for log in logs]
    if len(logs) == 2:
        parts.append(diff(*logs))
    return "\n\n".join(parts) + "\n"


def main(argv):
    if len(argv) < 2 or any(arg.startswith("-") for arg in argv[1:]):
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    try:
        sys.stdout.write(summarize(argv[1:]))
    except (OSError, LogError) as error:
        print(f"summarize.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
