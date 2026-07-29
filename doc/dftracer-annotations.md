# dftracer annotations

This branch carries dftracer instrumentation for Fluxion, gated by depth so you
can pick how much of the scheduler to trace. It is off by default and adds no
dependency on dftracer unless you turn it on.

    cmake -S . -B build -DDFTRACER_ANNOTATION_LEVEL=2 ...

Levels are cumulative — each keeps everything above it and adds its own tier:

| level | tier | what it covers | sites |
|---|---|---|---|
| 0 | off | nothing; no dftracer dependency at all | — |
| 1 | `QMANAGER` | queue manager: where jobs arrive and allocations go back out | 6 |
| 2 | `MATCH` | the resource module's request path — match, match_multi, cancel, feasibility, and the other RPC callbacks | 28 |
| 3 | `GRAPH` | graph construction and policy selection: readers, graph store, match-policy factory, jobspec parsing | 12 |
| 4 | `TRAVERSE` | the traversal itself, plus the standalone utilities (`resource-query`, `grug2dot`) | 10 |
| 5 | `PLANNER` | planner and string-interner leaf calls | 14 |

When dftracer is installed as a wheel it ships no CMake config file, so point the
build at it with `DFTRACER_ROOT` (the prefix containing `include/` and `lib/`):

    DFTRACER_ROOT=/usr/lib/python3/dist-packages/dftracer \
      cmake -S . -B build -DDFTRACER_ANNOTATION_LEVEL=5 ...

Configuring with a level above 0 and no dftracer available is a hard error rather
than a silent downgrade.

## Choosing a level, and turning off I/O interception

Measured on the flux-fiction rabbit300 emulation (300 jobs, 1153 nodes), all runs
on the same node from this tree with `CMAKE_BUILD_TYPE=Release`, level 0 as the
control:

| build | wall | vs level 0 | Fluxion events |
|---|---|---|---|
| level 0 | 62 s | 1.00x | — |
| level 2, `DFTRACER_DISABLE_IO=1` | 64 s | **1.03x** | 1,536 |
| level 2 | 128 s | 2.06x | 1,536 |
| level 5, `DFTRACER_DISABLE_IO=1` | 155 s | 2.50x | 2,570,816 |
| level 5 | 351 s | 5.66x | 2,605,883 |

Two things fall out of that.

**Set `DFTRACER_DISABLE_IO=1` unless you actually want I/O events.** Initializing
dftracer at all installs its gotcha POSIX/stdio interception, and that — not the
annotations — is most of the cost: it accounts for 64 of level 2's 66 seconds of
overhead, and 196 of level 5's 289. Disabling it changes the annotation event
count by ~1% (the difference is dftracer's own `POSIX/*` events), so with it set,
level 2 is effectively free.

**Level 5 is expensive even with I/O interception off**, because it is 2.6M
events, 74% of them from the string interner's `get_both`. That multiplier is
worse under emulation than it would be in a normal run: flux-fiction drives the
broker under `libfaketime` with `FAKETIME_NO_CACHE=1`, so every clock read a
`DFTracer` constructor and destructor performs becomes an `open`/`read`/`close`
of the stamp file on `/dev/shm` (~20 µs) instead of a vDSO call (~0.02 µs).
Per-string and per-vertex annotations are therefore roughly a thousand times more
expensive there than they look. Levels 1-4 leave those paths alone.

Level 2 is the sensible default for scheduler work: one event per job through
`match_multi_request_cb`, `feasibility_request_cb` and `cancel_request_cb`, at
no measurable cost.

## Adding an annotation

Name the tier; don't test the level directly.

```c++
#include "src/common/dftracer_annotation.hpp"

static int run_match (...)
{
    FLUX_DFT_FN (MATCH);
    FLUX_DFT_UPDATE (MATCH, "comp", "cpu");
    ...
}
```

Below its level a tier expands to nothing, so the function compiles unchanged and
references no dftracer symbol.

Standalone binaries call `FLUX_DFT_PROCESS_INIT ()` once at the top of `main`.
That both initializes dftracer and registers the finalize — dftracer only writes
its buffered events out on an explicit `finalize()`, so init alone leaves a
0-byte trace file.

## Tracing the modules

The broker modules must **not** use `FLUX_DFT_PROCESS_INIT`. flux-fiction unloads
and reloads Fluxion a few seconds into a run, and glibc runs a DSO's `atexit`
handlers at `dlclose` — a process-style finalize there closes the trace at the
configure-time unload and discards everything after it, while still producing a
file that looks complete. `src/common/dftracer_bootstrap.hpp` handles this by
counting module load generations in the environment; `sched-fluxion-resource`'s
`mod_main` is the single call site. See the header for the full reasoning.

## Regenerating the annotations

`scripts/port_annotations.py` re-applies the annotation set from an annotated tree
onto this one by matching function signatures, and
`scripts/apply_annotation_levels.py` converts the raw `DFTRACER_CPP_FUNCTION()`
form into the gated `FLUX_DFT_*` form using the directory→tier table at the top of
that script. Both are dry-run by default.
