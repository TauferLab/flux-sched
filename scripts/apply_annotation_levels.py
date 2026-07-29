#!/usr/bin/env python3
"""Rewrite raw dftracer annotations into depth-gated ones.

port_annotations.py reproduces what the annotation generator emits: a bare
DFTRACER_CPP_FUNCTION() plus a DFTRACER_CPP_FUNCTION_UPDATE(), and a
<dftracer/dftracer.h> include. That is all-or-nothing, and "all" is expensive --
level 5 on rabbit300 is 2.6M events, 74% of them from the string interner.

This assigns every site a tier based on which part of the scheduler its file
belongs to, and swaps the raw macros for the FLUX_DFT_* forms in
src/common/dftracer_annotation.hpp, which compile away below their level.

The mapping is by directory, because that is how Fluxion is layered: requests
arrive in qmanager, cross into the resource module, descend through the graph
readers and policies into the traversal, and bottom out in the planner and the
interner.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Longest prefix wins, so more specific paths can override their parent.
TIERS: list[tuple[str, str]] = [
    # 1 - the entry point: jobs arrive and allocations go back out
    ("qmanager/", "QMANAGER"),
    # 2 - the resource module's RPC surface: match, match_multi, cancel, ...
    ("resource/modules/", "MATCH"),
    # 3 - building the graph and choosing a policy
    ("resource/readers/", "GRAPH"),
    ("resource/store/", "GRAPH"),
    ("resource/policies/", "GRAPH"),
    ("resource/libjobspec/", "GRAPH"),
    # 4 - the traversal, and the standalone utilities that drive it directly
    ("resource/traversers/", "TRAVERSE"),
    ("resource/utilities/", "TRAVERSE"),
    # 5 - leaf calls, hit per-vertex and per-string
    ("resource/planner/", "PLANNER"),
    ("src/common/libintern/", "PLANNER"),
]

RAW_FN = "DFTRACER_CPP_FUNCTION();"
RAW_UPDATE = re.compile(r'DFTRACER_CPP_FUNCTION_UPDATE\("(\w+)", "(\w+)"\);')
RAW_INIT = re.compile(r"DFTRACER_C_INIT\s*\(NULL, NULL, NULL\);")
RAW_INCLUDE = "#include <dftracer/dftracer.h>"
NEW_INCLUDE = '#include "src/common/dftracer_annotation.hpp"'


def tier_for(rel: str) -> str | None:
    best: tuple[int, str] | None = None
    for prefix, tier in TIERS:
        if rel.startswith(prefix) and (best is None or len(prefix) > best[0]):
            best = (len(prefix), tier)
    return best[1] if best else None


def convert(path: Path, rel: str, tier: str, apply: bool) -> int:
    text = path.read_text()
    if RAW_FN not in text and RAW_INCLUDE not in text:
        return 0

    n = text.count(RAW_FN)
    text = text.replace(RAW_FN, f"FLUX_DFT_FN ({tier});")
    text = RAW_UPDATE.sub(lambda m: f'FLUX_DFT_UPDATE ({tier}, "{m.group(1)}", "{m.group(2)}");', text)
    text = RAW_INIT.sub("FLUX_DFT_PROCESS_INIT ();", text)
    text = text.replace(RAW_INCLUDE, NEW_INCLUDE)

    if apply:
        path.write_text(text)
    print(f"  {tier:<9} {rel}: {n} site(s)")
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, help="annotated flux-sched tree")
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    args = ap.parse_args()

    root = Path(args.tree).resolve()
    total = 0
    unmapped = []
    for src in sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.hpp")):
        if "/external/" in str(src) or "/build" in str(src):
            continue
        # Our own headers define the macros; they are not annotation sites.
        if src.name in ("dftracer_annotation.hpp", "dftracer_bootstrap.hpp"):
            continue
        rel = str(src.relative_to(root))
        text = src.read_text()
        if RAW_FN not in text and RAW_INCLUDE not in text:
            continue
        tier = tier_for(rel)
        if tier is None:
            unmapped.append(rel)
            continue
        total += convert(src, rel, tier, args.apply)

    if unmapped:
        print("\nno tier mapped for these annotated files -- add them to TIERS:")
        for rel in unmapped:
            print(f"  {rel}")
        return 1

    print(f"\n{total} annotation site(s) gated")
    print("dry run -- rerun with --apply" if not args.apply else "written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
