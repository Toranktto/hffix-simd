#!/usr/bin/env python3
"""Render README-shaped bench tables from Google Benchmark JSON.

    compare2upstream_render.py [--tables LIST] LABEL=PATH [LABEL=PATH ...]

First column is the baseline for (+X %) / (-X %) deltas."""

from __future__ import annotations

import argparse
import json
import re
import sys
from typing import Optional


def load(path: str) -> dict[str, dict[str, float]]:
    with open(path) as f:
        data = json.load(f)
    benches = data.get("benchmarks", [])

    out: dict[str, dict[str, float]] = {}

    # Prefer `_mean` aggregates (emitted when --benchmark_repetitions >= 2).
    for b in benches:
        if b.get("aggregate_name") != "mean":
            continue
        name = b.get("run_name") or b["name"].rsplit("_", 1)[0]
        out[name] = {
            "cpu_time": float(b.get("cpu_time", 0.0)),
            "items_per_second": float(b.get("items_per_second", 0.0)),
            "p95_ns": float(b.get("p95_ns", 0.0)),
            "p99_ns": float(b.get("p99_ns", 0.0)),
            "p999_ns": float(b.get("p999_ns", 0.0)),
            "max_ns": float(b.get("max_ns", 0.0)),
        }

    # Fall back to the single iteration row when no mean exists (reps == 1).
    # Multiple iteration rows per bench are possible under interleaving; keep
    # the first hit per name.
    for b in benches:
        if b.get("aggregate_name"):
            continue
        if b.get("run_type") != "iteration":
            continue
        name = b.get("run_name") or b["name"]
        if name in out:
            continue
        out[name] = {
            "cpu_time": float(b.get("cpu_time", 0.0)),
            "items_per_second": float(b.get("items_per_second", 0.0)),
            "p95_ns": float(b.get("p95_ns", 0.0)),
            "p99_ns": float(b.get("p99_ns", 0.0)),
            "p999_ns": float(b.get("p999_ns", 0.0)),
            "max_ns": float(b.get("max_ns", 0.0)),
        }
    return out


def first_hit(col: dict, names: list[str]) -> Optional[dict[str, float]]:
    for n in names:
        if n in col:
            return col[n]
    return None


MISSING = "—"


def fmt(val: float, unit: str) -> str:
    if val <= 0:
        return MISSING
    if unit == "msgs":
        return f"{val / 1e6:.3g} M msgs/s"
    if unit == "fields":
        return f"{val / 1e6:.3g} M f/s"
    if unit == "ns":
        return f"~{val:.3g} ns"
    return f"{val:.3g}"


def delta(value: float, baseline: float) -> str:
    if baseline <= 0:
        return ""
    pct = (value - baseline) / baseline * 100.0
    sign = "+" if pct >= 0 else "-"
    return f" ({sign}{abs(pct):.0f} %)"


def bold(cell: str) -> str:
    if cell == MISSING or cell.startswith("**"):
        return cell
    return f"**{cell}**"


def emit(headers: list[str], rows: list[list[str]]) -> None:
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    print()


def pick_winner(values: list[float], higher_better: bool) -> Optional[int]:
    valid = [(i, v) for i, v in enumerate(values) if v > 0]
    if len(valid) < 2:
        return None
    return (max if higher_better else min)(valid, key=lambda kv: kv[1])[0]


ITER_ROWS = [
    ("ScanMessages",
     ["BM_Parse_ScanMessages/synthetic"], "msgs"),
    ("IterAllFields",
     ["BM_Parse_IterAllFields/synthetic"], "fields"),
    ("FindCommonFields(5)",
     ["BM_Parse_FindCommonFields/synthetic"], "msgs"),
    ("FindManyFields(15)",
     ["BM_Parse_FindManyFields_Iter/synthetic",
      "BM_Parse_FindManyFields/synthetic"], "msgs"),
    ("FindLotsFields(30)",
     ["BM_Parse_FindLotsFields_Iter/synthetic",
      "BM_Parse_FindLotsFields/synthetic"], "msgs"),
]

INDEXED_ROWS = [
    ("BuildFieldIndex (no lookups)",
     "BM_Parse_BuildFieldIndex/synthetic", None, "msgs"),
    ("FindCommonFields(5) idx",
     "BM_Parse_FindCommonFields_Indexed/synthetic",
     "BM_Parse_FindCommonFields/synthetic", "msgs"),
    ("FindManyFields(15) idx",
     "BM_Parse_FindManyFields_Indexed/synthetic",
     "BM_Parse_FindManyFields_Iter/synthetic", "msgs"),
    ("FindLotsFields(30) idx",
     "BM_Parse_FindLotsFields_Indexed/synthetic",
     "BM_Parse_FindLotsFields_Iter/synthetic", "msgs"),
]

SINGLE_ROWS = [
    "BM_WriteLogon",
    "BM_WriteNewOrder",
    "BM_ReadMessageScan",
    "BM_ReadMessageFindFields",
    "BM_RoundTrip",
    "BM_ParseTimestampMillis",
    "BM_ParseTimestampNanos",
]

def discover_ns(columns: list[tuple[str, dict]], *templates: str) -> list[int]:
    pats = [re.compile(re.escape(t).replace(r"\{n\}", r"(\d+)") + r"$")
            for t in templates]
    ns: set[int] = set()
    for _, c in columns:
        for k in c:
            for p in pats:
                m = p.match(k)
                if m:
                    ns.add(int(m.group(1)))
    return sorted(ns)


def render_iterator(columns: list[tuple[str, dict]]) -> None:
    print("Iterator path (like-for-like, first column is baseline):\n")
    out = []
    for label, names, unit in ITER_ROWS:
        entries = [first_hit(c, names) for _, c in columns]
        baseline = entries[0]
        vals = []
        cells = []
        for i, e in enumerate(entries):
            if not e:
                cells.append(MISSING); vals.append(0.0); continue
            v = e["items_per_second"]
            cells.append(fmt(v, unit) + (delta(v, baseline["items_per_second"])
                                         if i > 0 and baseline else ""))
            vals.append(v)
        w = pick_winner(vals, higher_better=True)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(["Benchmark"] + [l for l, _ in columns], out)


def render_indexed(columns: list[tuple[str, dict]]) -> None:
    print("Indexed (`field_index_buffer<N>` path). "
          "Delta vs same-column iterator on the same workload:\n")
    out = []
    for label, idx_name, iter_name, unit in INDEXED_ROWS:
        vals = []
        cells = []
        for _, c in columns:
            e = c.get(idx_name)
            if not e:
                cells.append(MISSING); vals.append(0.0); continue
            v = e["items_per_second"]
            cell = fmt(v, unit)
            if iter_name:
                base = c.get(iter_name)
                if base:
                    cell += delta(v, base["items_per_second"])
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=True)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(["Benchmark"] + [l for l, _ in columns], out)


def render_single(columns: list[tuple[str, dict]]) -> None:
    print("Single-message hot path (in-cache):\n")
    out = []
    for name in SINGLE_ROWS:
        entries = [c.get(name) for _, c in columns]
        baseline = entries[0]
        vals = []
        cells = []
        for i, e in enumerate(entries):
            if not e:
                cells.append(MISSING); vals.append(0.0); continue
            v = e["cpu_time"]
            cells.append(fmt(v, "ns") + (delta(v, baseline["cpu_time"])
                                         if i > 0 and baseline else ""))
            vals.append(v)
        w = pick_winner(vals, higher_better=False)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{name}`"] + cells)
    emit(["Benchmark"] + [l for l, _ in columns], out)


def _iter_vs_idx(title: str,
                 columns: list[tuple[str, dict]],
                 iter_tpl: str,
                 idx_tpl: str,
                 unit: str) -> None:
    counts = discover_ns(columns, iter_tpl, idx_tpl)
    if not counts:
        return
    key = "cpu_time" if unit == "ns" else "items_per_second"
    higher_better = unit != "ns"
    for clabel, cdata in columns:
        rows = []
        any_pair = False
        for n in counts:
            it = cdata.get(iter_tpl.format(n=n))
            ix = cdata.get(idx_tpl.format(n=n))
            it_v = it[key] if it else 0.0
            ix_v = ix[key] if ix else 0.0
            it_cell = fmt(it_v, unit)
            ix_cell = fmt(ix_v, unit)
            ratio_cell = MISSING
            if it_v > 0 and ix_v > 0:
                any_pair = True
                ratio = (ix_v / it_v) if higher_better else (it_v / ix_v)
                ratio_cell = f"{ratio:.2f}x"
                if higher_better:
                    if it_v > ix_v: it_cell = bold(it_cell)
                    elif ix_v > it_v: ix_cell = bold(ix_cell)
                else:
                    if it_v < ix_v: it_cell = bold(it_cell)
                    elif ix_v < it_v: ix_cell = bold(ix_cell)
            rows.append([str(n), it_cell, ix_cell, ratio_cell])
        if not any_pair:
            continue
        print(title.format(label=clabel))
        print()
        emit(["Lookups", "iterator", "indexed", "idx/iter"], rows)


def render_lookups(columns: list[tuple[str, dict]]) -> None:
    _iter_vs_idx(
        "Per-lookup-count break-even (`{label}`, single NewOrder, in-cache):",
        columns,
        "BM_FindManyFields_Iterator/{n}",
        "BM_FindManyFields_Indexed/{n}",
        "ns")


def render_amortize(columns: list[tuple[str, dict]]) -> None:
    _iter_vs_idx(
        "Index amortization on dataset (`{label}`):",
        columns,
        "BM_Parse_FindN_Iter/{n}/synthetic",
        "BM_Parse_FindN_Indexed/{n}/synthetic",
        "msgs")


SEQ_RAND_ROWS = [
    ("Sequential_Iter",     "BM_Parse_Sequential_Iter/synthetic"),
    ("Random_Iter",         "BM_Parse_Random_Iter/synthetic"),
    ("Sequential_Indexed",  "BM_Parse_Sequential_Indexed/synthetic"),
    ("Random_Indexed",      "BM_Parse_Random_Indexed/synthetic"),
]


def render_seq_rand(columns: list[tuple[str, dict]]) -> None:
    print("Sequential vs random tag read on the dataset "
          "(first column is baseline; throughput in `M msgs/s`):\n")
    out = []
    for label, name in SEQ_RAND_ROWS:
        entries = [c.get(name) for _, c in columns]
        baseline = entries[0]
        vals = []
        cells = []
        for i, e in enumerate(entries):
            if not e or e["items_per_second"] <= 0:
                cells.append(MISSING)
                vals.append(0.0)
                continue
            v = e["items_per_second"]
            cell = fmt(v, "msgs")
            if i > 0 and baseline and baseline.get("items_per_second", 0.0) > 0:
                cell += delta(v, baseline["items_per_second"])
            cells.append(cell)
            vals.append(v)
        w = pick_winner(vals, higher_better=True)
        if w is not None:
            cells[w] = bold(cells[w])
        out.append([f"`{label}`"] + cells)
    emit(["Benchmark"] + [l for l, _ in columns], out)


TAIL_ROWS = [
    ("Sequential_Iter",     "BM_Parse_TailLatency_Sequential_Iter/synthetic"),
    ("Random_Iter",         "BM_Parse_TailLatency_Random_Iter/synthetic"),
    ("Sequential_Indexed",  "BM_Parse_TailLatency_Sequential_Indexed/synthetic"),
    ("Random_Indexed",      "BM_Parse_TailLatency_Random_Indexed/synthetic"),
]
TAIL_PCTS = [
    ("p95",  "p95_ns"),
    ("p99",  "p99_ns"),
    ("p999", "p999_ns"),
    ("max",  "max_ns"),
]


def render_tail(columns: list[tuple[str, dict]]) -> None:
    any_emitted = False
    for clabel, cdata in columns:
        rows = []
        for label, name in TAIL_ROWS:
            e = cdata.get(name)
            if not e or e.get("p95_ns", 0.0) <= 0:
                continue
            cells = [f"`{label}`"]
            for _, key in TAIL_PCTS:
                v = e.get(key, 0.0)
                cells.append(fmt(v, "ns") if v > 0 else MISSING)
            rows.append(cells)
        if not rows:
            continue
        if any_emitted:
            print()
        any_emitted = True
        print(f"Tail latency per message (`{clabel}`, includes "
              f"~20-30 ns clock::now() probe; ns):\n")
        emit(["Benchmark"] + [p for p, _ in TAIL_PCTS], rows)


def parse_column(s: str) -> tuple[str, dict]:
    if "=" not in s:
        raise argparse.ArgumentTypeError(
            f"column spec must be LABEL=PATH, got {s!r}")
    label, path = s.split("=", 1)
    return label, load(path)


RENDERERS = {
    "iterator": render_iterator,
    "indexed": render_indexed,
    "single": render_single,
    "lookups": render_lookups,
    "amortize": render_amortize,
    "seq_rand": render_seq_rand,
    "tail": render_tail,
}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("columns", nargs="+", type=parse_column,
                    metavar="LABEL=PATH",
                    help="Bench JSON columns; first is baseline for deltas.")
    ap.add_argument("--tables", default=",".join(RENDERERS),
                    help=f"Comma list. Options: {','.join(RENDERERS)}. "
                         f"Default: all.")
    args = ap.parse_args(argv)

    columns: list[tuple[str, dict]] = args.columns
    wanted = [t.strip() for t in args.tables.split(",") if t.strip()]
    for name in wanted:
        if name not in RENDERERS:
            print(f"unknown table: {name!r}", file=sys.stderr)
            return 2
        RENDERERS[name](columns)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
