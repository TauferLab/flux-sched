#!/usr/bin/env python3
"""Port the dftracer annotations from upstream flux-sched onto the flux-fiction fork.

The tree we were handed (hari/annotated-flux-sched.tar.gz) is annotated upstream
v0.53.0. flux-fiction can't use it: it drives the scheduler through a
``sched.quiescent`` RPC that only exists on the fork in ff-podman/flux-sched
(branch debug_erange, v0.51.0-based), so a run against upstream dies with
"Unknown service method 'sched.quiescent'".

The annotations themselves are mechanical -- an include, and two lines at the top
of each instrumented function -- so rather than force a textual patch across two
releases of divergence, this re-applies them by matching each instrumented
function's *signature text* in the target tree. Functions that moved or changed
shape between the two versions simply report as misses instead of corrupting the
file.

The 54 auto-injected per-directory CMake blocks are deliberately NOT ported: a
single project-wide block in the root CMakeLists does the same job (and does it
correctly with --as-needed). See scripts/add_cmake_block.py.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MARKER = "DFTRACER_CPP_FUNCTION();"
UPDATE_RE = re.compile(r'DFTRACER_CPP_FUNCTION_UPDATE\("comp", "(\w+)"\);')
INCLUDE_LINE = "#include <dftracer/dftracer.h>"


def collect_sites(path: Path) -> list[tuple[list[str], str, str]]:
    """Extract (signature_lines, indent, comp) for every annotated function.

    The annotator always inserts directly after a function's opening brace, so
    walk back from the marker over the contiguous block of declaration lines --
    that block is what we go looking for in the target file.
    """
    lines = path.read_text().splitlines()
    sites = []
    for i, line in enumerate(lines):
        if MARKER not in line:
            continue
        indent = line[: len(line) - len(line.lstrip())]

        comp = ""
        if i + 1 < len(lines):
            m = UPDATE_RE.search(lines[i + 1])
            if m:
                comp = m.group(1)

        # The line above the marker closes the signature with '{'.
        j = i - 1
        if j < 0 or not lines[j].rstrip().endswith("{"):
            print(f"  !! {path.name}:{i + 1}: marker not directly after '{{', skipping")
            continue

        sig: list[str] = []
        while j >= 0:
            cur = lines[j]
            sig.insert(0, cur)
            stripped = cur.strip()
            # Walk back until the previous line ends a different construct.
            prev = lines[j - 1].strip() if j - 1 >= 0 else ""
            if (
                not prev
                or prev.endswith(("}", ";", "{"))
                or prev.startswith(("//", "/*", "*", "#"))
            ):
                break
            j -= 1
            if len(sig) > 12:
                break
        sites.append((sig, indent, comp))
    return sites


def find_signature(target: list[str], sig: list[str]) -> int | None:
    """Index of the line after the matching signature's opening brace, or None."""
    want = [s.strip() for s in sig]
    hits = []
    for i in range(len(target) - len(want) + 1):
        if [t.strip() for t in target[i : i + len(want)]] == want:
            hits.append(i + len(want))
    if len(hits) != 1:
        return None
    return hits[0]


def include_insert_index(lines: list[str]) -> int | None:
    """Last top-level #include, skipping any inside an `extern "C" {` block.

    dftracer.h declares a C++ class, so landing it inside `extern "C"` -- which
    several fluxion sources open around the flux-core headers -- would not
    compile.
    """
    depth = 0
    in_extern_c = False
    extern_depth = 0
    best = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r'^extern\s+"C"\s*\{', stripped):
            in_extern_c = True
            extern_depth = depth
            depth += 1
            continue
        depth += line.count("{") - line.count("}")
        if in_extern_c and depth <= extern_depth:
            in_extern_c = False
        if stripped.startswith("#include") and not in_extern_c:
            best = i
    return best


def port_file(src: Path, dst: Path, apply: bool) -> tuple[int, int]:
    sites = collect_sites(src)
    if not sites:
        return (0, 0)
    if not dst.exists():
        print(f"  -- {dst}: not present in target tree, skipping {len(sites)} site(s)")
        return (0, len(sites))

    lines = dst.read_text().splitlines()
    if any(MARKER in line for line in lines):
        print(f"  == {dst}: already annotated, skipping")
        return (0, 0)

    # Insert from the bottom so earlier indices stay valid.
    placements = []
    misses = 0
    for sig, indent, comp in sites:
        idx = find_signature(lines, sig)
        if idx is None:
            misses += 1
            print(f"  !! {dst.name}: no unique match for `{sig[0].strip()[:70]}`")
            continue
        placements.append((idx, indent, comp))

    for idx, indent, comp in sorted(placements, reverse=True):
        ins = [f"{indent}{MARKER}"]
        if comp:
            ins.append(f'{indent}DFTRACER_CPP_FUNCTION_UPDATE("comp", "{comp}");')
        lines[idx:idx] = ins

    if placements and INCLUDE_LINE not in "\n".join(lines):
        inc = include_insert_index(lines)
        if inc is None:
            print(f"  !! {dst.name}: nowhere safe to add the dftracer include")
            return (0, misses + len(placements))
        lines.insert(inc + 1, INCLUDE_LINE)

    if apply and placements:
        dst.write_text("\n".join(lines) + "\n")
    print(f"  ++ {dst.relative_to(dst.parents[len(dst.parts) - 1])}: {len(placements)} site(s)"
          if False else f"  ++ {dst.name}: {len(placements)} site(s)")
    return (len(placements), misses)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="annotated upstream tree")
    ap.add_argument("--target", required=True, help="flux-fiction's flux-sched fork")
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    args = ap.parse_args()

    src_root = Path(args.source).resolve()
    dst_root = Path(args.target).resolve()

    total, missed = 0, 0
    for src in sorted(src_root.rglob("*.cpp")) + sorted(src_root.rglob("*.hpp")):
        if "/external/" in str(src) or "/build" in str(src):
            continue
        rel = src.relative_to(src_root)
        n, m = port_file(src, dst_root / rel, args.apply)
        total += n
        missed += m

    print(f"\nported {total} annotation site(s); {missed} miss(es)")
    print("dry run -- rerun with --apply" if not args.apply else "written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
