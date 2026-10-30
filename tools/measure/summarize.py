#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Summarize the player's measure logs as markdown.

usage: summarize.py LOG.csv [LOG.csv ...]

A log is the player's (src/volumetric_kit/player/measure_log.hpp, format 1
to 5), as vp_probe and the iOS app's measure launch write it. Per log,
under what it measured (format 3 adds whether vertices were shared; format 4
names the grid's commit where the earlier named recon's), and
after its device the machine's load before the run if a <log>.load beside it
says (the runner's mac mode writes one): per stage of the worker's, then of
the renderer's (format 2), the mean, p50 and p95 of its host (busy), wait and
GPU milliseconds, and how many of its frames had the GPU measured; then the
worker's device counts per frame (format 2 adds its dispatches and barriers)
and, from format 3, its mesh's vertices; then the frames' figures: the device
time of a timed frame (its stages' GPU, breakdowns aside) and, from format 2,
its queue idle and queue latency, its latency from due to the present call,
or for a frame with no present (the worker alone) from due to taken, apart,
and its legs (from format 5, a live frame's wait as bytes first, and its
publish leg from its release, which a file's due is), then from format 5 a
live frame's playout delay, its target and how late it was taken after its
present time; the rounds its decode ran on the device; and from format 5 the
needs that set the delay. Given two logs, a
before/after diff of the first and the second follows; a figure one log lacks
shows as "-". A log of several passes leaves out its first, which warms up.
p50 and p95 are nearest-rank. A stage first seen in a later frame, or only in
the second log, goes after the row it follows there, so a breakdown stays
under its stage, as the player's stats place it (PlaybackStats::stage_means).
"""

from __future__ import annotations

import csv
import math
import os
import sys
from dataclasses import dataclass, field

FORMAT_1 = "volumetric_kit_player measure 1"
FORMAT_2 = "volumetric_kit_player measure 2"
FORMAT_3 = "volumetric_kit_player measure 3"
FORMAT_4 = "volumetric_kit_player measure 4"
FORMAT_5 = "volumetric_kit_player measure 5"
FORMATS = {FORMAT_1: 1, FORMAT_2: 2, FORMAT_3: 3, FORMAT_4: 4, FORMAT_5: 5}
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
# Format 2's further keys, and format 3's; a phone's log adds thermal and
# low_power.
HEADER_KEYS_2 = HEADER_KEYS + ("rings", "tile_depth", "labels")
HEADER_KEYS_3 = HEADER_KEYS_2 + ("share_vertices",)
# Format 4's: the grid's commit in place of recon's, which the player no
# longer builds on.
HEADER_KEYS_4 = tuple("grid" if key == "recon" else key
                      for key in HEADER_KEYS_3)
OPTIONAL_KEYS = ("rings", "tile_depth", "labels", "share_vertices", "thermal",
                 "low_power")
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
COLUMNS_2 = [
    "pass",
    "frame",
    "type",
    "bytes",
    "triangles",
    "table",
    "stage",
    "host_ms",
    "wait_ms",
    "gpu_ms",
    "device",
    "submits",
    "host_waits",
    "blocked_ns",
    "staging_buffers",
    "dispatches",
    "barriers",
    "deblock",
    "exact_blocks",
    "rounds",
    "idle_ms",
    "queue_ms",
    "publish_ms",
    "take_ms",
    "present_ms",
]
# Format 3's frame row adds its mesh's vertices and its record's set.
COLUMNS_3 = COLUMNS_2 + ["vertices", "set"]
# Format 5's adds a live frame's wait as bytes and its playout as taken.
PLAYOUT = ("delay_ms", "target_ms", "late_ms")
COLUMNS_5 = COLUMNS_3 + ["buffer_ms", "delay_ms", "target_ms", "need",
                         "late_ms"]
NEEDS = ("none", "arrival", "program")
DEVICES = ("none", "measured", "not_timed", "unavailable")
COUNTS = ("submits", "host_waits", "blocked_ns", "staging_buffers")
COUNTS_2 = COUNTS + ("dispatches", "barriers")
COUNTS_3 = COUNTS_2 + ("vertices",)
# Each format's columns, header keys and counts per frame.
LAYOUTS = {1: (COLUMNS, HEADER_KEYS, COUNTS),
           2: (COLUMNS_2, HEADER_KEYS_2, COUNTS_2),
           3: (COLUMNS_3, HEADER_KEYS_3, COUNTS_3),
           4: (COLUMNS_3, HEADER_KEYS_4, COUNTS_3),
           5: (COLUMNS_5, HEADER_KEYS_4, COUNTS_3)}
LEGS = ("publish_ms", "take_ms", "present_ms")
# The frames' figures, in the order the tables show them: a frame's device
# time, read from its stages in either format, then format 2's own: the
# latency of a frame presented, and of one taken with no present, apart;
# its legs, format 5's buffer first; and format 5's playout.
FIGURES = ("gpu_ms", "idle_ms", "queue_ms", "latency_ms",
           "taken_latency_ms", "buffer_ms") + LEGS + PLAYOUT
TABLES = ("worker", "render")
BREAKDOWN = "  .."


class LogError(ValueError):
    """A file that is not a measure log of a format this reads."""


@dataclass
class Stage:
    """A stage's figures over the frames kept."""

    host: list = field(default_factory=list)
    wait: list = field(default_factory=list)
    gpu: list = field(default_factory=list)
    devices: dict = field(default_factory=dict)  # state -> frames


@dataclass
class Table:
    """A table's stages, in place order (place)."""

    order: list = field(default_factory=list)
    stages: dict = field(default_factory=dict)


@dataclass
class Log:
    """A log's header and its figures over the passes kept."""

    path: str
    header: dict
    format: int
    passes: list  # every pass in the log
    kept: list  # the passes the figures are over
    frames: int = 0
    tables: dict = field(default_factory=lambda: {t: Table() for t in TABLES})
    counts: dict = field(default_factory=lambda: {key: [] for key in COUNTS_3})
    figures: dict = field(default_factory=lambda: {k: [] for k in FIGURES})
    rounds: dict = field(default_factory=dict)  # rounds -> frames
    needs: dict = field(default_factory=dict)  # format 5: need -> frames
    load: str = ""  # the machine's load before the run (read_load)

    @property
    def order(self):
        return self.tables["worker"].order

    @property
    def stages(self):
        return self.tables["worker"].stages


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


def read_header(path, lines):
    """The log's `# key: value` lines, its format, and where its CSV begins."""
    header = {}
    at = 0
    while at < len(lines) and lines[at].startswith("#"):
        key, colon, value = lines[at][1:].strip().partition(":")
        if not colon:
            raise LogError(f"{path}: header line {at + 1} is not '# key: value'")
        header[key.strip()] = value.strip()
        at += 1
    if "format" in header and header["format"] not in FORMATS:
        raise LogError(f"{path}: format {header['format']!r}, not "
                       f"{FORMAT_1!r}, {FORMAT_2!r}, {FORMAT_3!r}, "
                       f"{FORMAT_4!r} or {FORMAT_5!r}")
    version = FORMATS.get(header.get("format"), 1)
    _, keys, _ = LAYOUTS[version]
    missing = [key for key in keys if key not in header]
    if missing:
        raise LogError(f"{path}: the header lacks {', '.join(missing)}")
    return header, version, at


def read_stage(log, row, path, at):
    """A stage row into its table; the index after it in its frame's order."""
    device = row["device"]
    if device not in DEVICES:
        raise LogError(f"{path}: device {device!r} is none of {DEVICES}")
    if (device == "measured") != (row["gpu_ms"] != ""):
        raise LogError(
            f"{path}: {row['stage']!r} has gpu_ms {row['gpu_ms']!r} "
            f"with device {device}"
        )
    table_name = row.get("table", "worker")
    if table_name not in TABLES:
        raise LogError(f"{path}: table {table_name!r} is none of {TABLES}")
    table = log.tables[table_name]
    name = row["stage"]
    at[table_name] = place(table.order, name, at[table_name])
    stage = table.stages.setdefault(name, Stage())
    stage.host.append(number(row, "host_ms", path))
    stage.wait.append(number(row, "wait_ms", path))
    gpu = None
    if device == "measured":
        gpu = number(row, "gpu_ms", path)
        stage.gpu.append(gpu)
    stage.devices[device] = stage.devices.get(device, 0) + 1
    return gpu


def read_frame(log, row, path, frame_gpu):
    """A frame's row: its counts and, from format 2, its figures; from
    format 3 its vertices among its counts, and its set, a number or empty."""
    log.frames += 1
    _, _, counts = LAYOUTS[log.format]
    for key in counts:
        log.counts[key].append(number(row, key, path, int))
    if frame_gpu is not None:
        log.figures["gpu_ms"].append(frame_gpu)
    if log.format == 1:
        return
    if log.format >= 3 and row["set"] != "":
        number(row, "set", path, int)
    for key in ("idle_ms", "queue_ms") + LEGS:
        if row[key] != "":
            log.figures[key].append(number(row, key, path))
    # Format 5, live: its wait as bytes leads its legs; a file's has none.
    buffer = 0.0
    if log.format >= 5:
        buffer = read_playout(log, row, path)
    if all(row[key] != "" for key in LEGS):
        log.figures["latency_ms"].append(
            buffer + sum(number(row, key, path) for key in LEGS))
    elif row["publish_ms"] != "" and row["take_ms"] != "":
        log.figures["taken_latency_ms"].append(
            buffer + number(row, "publish_ms", path)
            + number(row, "take_ms", path))
    rounds = number(row, "rounds", path, int)
    log.rounds[rounds] = log.rounds.get(rounds, 0) + 1


def read_playout(log, row, path):
    """A format-5 frame row's live figures and need; its buffer leg, 0 for a
    file's, whose five are empty."""
    for key in ("buffer_ms",) + PLAYOUT:
        if row[key] != "":
            log.figures[key].append(number(row, key, path))
    need = row["need"]
    if need != "":
        if need not in NEEDS:
            raise LogError(f"{path}: need {need!r} is none of {NEEDS}")
        log.needs[need] = log.needs.get(need, 0) + 1
    return number(row, "buffer_ms", path) if row["buffer_ms"] != "" else 0.0


def read_log(path):
    """The log at path, or LogError naming what is wrong with it."""
    with open(path, newline="", encoding="utf-8") as file:
        lines = file.read().splitlines()
    header, version, at = read_header(path, lines)
    columns, _, _ = LAYOUTS[version]
    reader = csv.DictReader(lines[at:])
    if reader.fieldnames != columns:
        raise LogError(f"{path}: columns {reader.fieldnames}, not {columns}")
    rows = list(reader)
    if not rows:
        raise LogError(f"{path}: no frames")
    passes = sorted({number(row, "pass", path, int) for row in rows})
    log = Log(path, header, version, passes,
              passes[1:] if len(passes) > 1 else passes, load=read_load(path))
    at = {table: 0 for table in TABLES}  # where a frame's next new stage goes
    frame_gpu = None  # the frame's worker stages' GPU, breakdowns aside
    for row in rows:
        if number(row, "pass", path, int) not in log.kept:
            continue
        if row["stage"] == "":  # a frame's row ends it
            read_frame(log, row, path, frame_gpu)
            at = {table: 0 for table in TABLES}
            frame_gpu = None
            continue
        gpu = read_stage(log, row, path, at)
        if (gpu is not None and row.get("table", "worker") == "worker"
                and not row["stage"].startswith(BREAKDOWN)):
            frame_gpu = (frame_gpu or 0.0) + gpu
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


def stage_table(table, unit):
    """A table's markdown rows, per `unit`."""
    out = [
        f"| stage (ms per {unit}) | host mean | p50 | p95 "
        "| wait mean | p50 | p95 | GPU mean | p50 | p95 | GPU measured |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for name in table.order:
        stage = table.stages[name]
        cells = (figures(stage.host) + figures(stage.wait)
                 + figures(stage.gpu) + [timed_cell(stage)])
        out.append(f"| {name} | " + " | ".join(cells) + " |")
    return out


def figure_label(key):
    """A frame figure's label in a table."""
    return {
        "gpu_ms": "device time ms (timed frames)",
        "idle_ms": "queue idle ms",
        "queue_ms": "queue latency ms",
        "latency_ms": "latency ms (due -> present)",
        "taken_latency_ms": "latency ms (due -> taken; no present)",
        "buffer_ms": "  ..due -> released ms (live)",
        "publish_ms": "  ..released (a file's due) -> published ms",
        "take_ms": "  ..published -> taken ms",
        "present_ms": "  ..taken -> presented ms",
        "delay_ms": "playout delay ms (live, as taken)",
        "target_ms": "  ..its target ms",
        "late_ms": "taken after its present time ms",
    }[key]


def summary(log):
    """The markdown for one log."""
    h = log.header
    sibling = "grid" if log.format >= 4 else "recon"
    settings = [f"{key} {h[key]}" for key in OPTIONAL_KEYS if key in h]
    out = [
        f"## {h['stream']} · timing {h['timing']} · {h['device'].split(';')[0]}",
        "",
        f"- device: {h['device']}",
        *([f"- load: {log.load}"] if log.load else []),
        *([f"- {', '.join(settings)}"] if settings else []),
        f"- run: {h['run']}",
        f"- build: player {h['player']} ({h['build']}), core {h['core']}, "
        f"{sibling} {h[sibling]}, gfx {h['gfx']}",
        f"- {h['utc']} · {passes_text(log)} · {log.frames} frames · {log.path}",
        "",
        *stage_table(log.tables["worker"], "stream frame"),
    ]
    if log.tables["render"].order:
        out += ["", *stage_table(log.tables["render"], "display frame")]
    out += [
        "",
        "| per frame | mean | p50 | p95 |",
        "|---|---:|---:|---:|",
    ]
    for key in LAYOUTS[log.format][2]:
        label, values = count(log, key)
        out.append(f"| {label} | " + " | ".join(figures(values, 2)) + " |")
    for key in FIGURES:
        if log.figures[key]:
            out.append(f"| {figure_label(key)} | "
                       + " | ".join(figures(log.figures[key])) + " |")
    if log.rounds:
        out += ["", "- rounds on the device: " + ", ".join(
            f"{rounds}: {frames}" for rounds, frames in sorted(log.rounds.items()))
            + " frames"]
    if log.needs:
        out += ["- delay set by: " + ", ".join(
            f"{need}: {log.needs[need]}" for need in NEEDS if need in log.needs)
            + " frames"]
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


def table_diff(before, after, unit):
    """The markdown rows comparing two tables' means, stage by stage."""
    names = list(before.order)
    at = 0
    for name in after.order:
        at = place(names, name, at)
    out = [f"| stage (ms per {unit}) | host | wait | GPU |", "|---|---|---|---|"]
    empty = Stage()
    for name in names:
        b = before.stages.get(name, empty)
        a = after.stages.get(name, empty)
        out.append(f"| {name} | {change(b.host, a.host)} | "
                   f"{change(b.wait, a.wait)} | {change(b.gpu, a.gpu)} |")
    return out


def run_text(log):
    """A log's player, run and timing, and from format 3 its vertex sharing,
    for a diff's line naming it."""
    h = log.header
    text = f"player {h['player']}, {h['run']}, timing {h['timing']}"
    if "share_vertices" in h:
        text += f", share_vertices {h['share_vertices']}"
    return text


def diff(before, after):
    """The markdown comparing two logs' means, stage by stage."""
    out = [
        "## Before -> after (means)",
        "",
        f"- before: {before.path} ({run_text(before)})",
        f"- after: {after.path} ({run_text(after)})",
        "",
        *table_diff(before.tables["worker"], after.tables["worker"],
                    "stream frame"),
    ]
    if before.tables["render"].order or after.tables["render"].order:
        out += ["", *table_diff(before.tables["render"],
                                after.tables["render"], "display frame")]
    out += ["", "| per frame | before -> after |", "|---|---|"]
    for key in COUNTS_3:
        label, b = count(before, key)
        a = count(after, key)[1]
        if key in COUNTS_2 or b or a:
            out.append(f"| {label} | {change(b, a)} |")
    for key in FIGURES:
        if before.figures[key] or after.figures[key]:
            out.append(f"| {figure_label(key)} | "
                       f"{change(before.figures[key], after.figures[key])} |")
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
