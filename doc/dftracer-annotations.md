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
| 2 | `MATCH` | the resource module's request path — match, match_multi, cancel, feasibility, and the other RPC callbacks | 29 |
| 3 | `GRAPH` | graph construction and policy selection: readers, graph store, match-policy factory, jobspec parsing | 12 |
| 4 | `TRAVERSE` | mostly the standalone utilities (`resource-query`, `grug2dot`) — see the caveat below | 10 |
| 5 | `PLANNER` | planner and string-interner leaf calls | 14 |

When dftracer is installed as a wheel it ships no CMake config file, so point the
build at it with `DFTRACER_ROOT` (the prefix containing `include/` and `lib/`):

    DFTRACER_ROOT=/usr/lib/python3/dist-packages/dftracer \
      cmake -S . -B build -DDFTRACER_ANNOTATION_LEVEL=5 ...

Configuring with a level above 0 and no dftracer available is a hard error rather
than a silent downgrade.

**Level 4 does not instrument the in-broker traversal.** Nine of its ten sites
are in `resource-query.cpp`, `grug2dot.cpp` and `command.cpp`, which are
standalone binaries that never load in the broker; the only site on the module
path is the `find_parent_edge` helper in `dfu_impl.cpp`. `dfu.cpp` — the
traversal driver, where `schedule()` and the reservation search live — carries
no annotation at any level, deliberately: annotating it would put a DFTracer
constructor and destructor inside the per-candidate-start-time loop, which under
flux-fiction's `FAKETIME_NO_CACHE=1` broker costs ~20 µs per clock read. The
counters described under **Per-job match attribution** exist so that the
reservation search can be measured without paying for that.

## Choosing a level, and turning off I/O interception

Measured on the flux-fiction rabbit300 emulation (300 jobs, 1153 nodes), all runs
on the same node from this tree with `CMAKE_BUILD_TYPE=Release`, level 0 as the
control:

| build | wall | vs level 0 | Fluxion events |
|---|---|---|---|
| level 0 | 62 s | 1.00x | — |
| level 2, `DFTRACER_DISABLE_IO=1` | 64 s | **1.03x** | 1,536 |
| level 2 | 128 s | 2.06x | 1,536 |
| level 5, `DFTRACER_DISABLE_IO=1` | 155 s | 2.50x | 2,570,814 |
| level 5 | 351 s | 5.66x | 2,570,814 + 8 `POSIX` |

Two things fall out of that.

**Set `DFTRACER_DISABLE_IO=1` unless you actually want I/O events.** Initializing
dftracer installs gotcha wrappers around the POSIX and stdio calls, and that --
not the annotations -- is most of the cost: 64 of level 2's 66 seconds of
overhead, and 196 of level 5's 289. It buys nothing here. The two level 5 traces
are identical at 2,570,814 annotation events; the whole difference in content is
8 `POSIX/execv` events, for 196 seconds. dftracer only records I/O under
DFTRACER_DATA_DIR, so with that pointed at the trace directory every syscall in
the broker pays for a wrapper that then declines to record anything. With it set,
level 2 is effectively free.

**Level 5 is expensive even with I/O interception off**, because it is 2.57M
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

## Per-job match attribution

At level 2 and above, `run_match()` emits a `CPP_APP` event carrying five
metadata fields:

| field | type | meaning |
|---|---|---|
| `jobid` | string | the job this match considered; `-1` for a feasibility probe |
| `cmd` | string | the op as requested. For the backfill policies this is the reserve discriminator: `reapi_module_t::match_allocate_multi()` sends `allocate_orelse_reserve` when the queue policy set `m_try_reserve`, and `allocate_with_satisfiability` when it did not |
| `outcome` | string | `allocated`, `reserved`, `satisfiable`, `busy`, `unsatisfiable`, or one of the error forms `match-error` / `invalid-cmd` / `emit-failed` / `track-failed` |
| `resv_attempts` | int | reservation attempts at candidate future start times inside this match |
| `sched_iters` | int | all traversals inside this match, including the initial allocation attempt |

Enable metadata when recording traces:

    export DFTRACER_INC_METADATA=1

`run_match()` is reached once for each allocation or reservation attempt,
including each job inside a `match_multi` RPC. The outer
`match_multi_request_cb` event remains a batch-level timing span and has no
single job ID. The feasibility service calls `run_match()` with `jobid=-1`;
exclude that sentinel when counting attempts for real jobs. A raw `.pfw.gz`
trace can be filtered with:

    zcat trace.pfw.gz | rg '"name":"run_match".*"jobid":"1234"' | wc -l

### Do not use the match count as a proxy for reservation work

**Every backfill policy issues exactly one match per pending job per scheduling
pass.** `queue_policy_bf_base_t::next_match_iter()` sends one single-job
`match_allocate_multi` per job and walks the pending queue; policy enters only
through `m_try_reserve = m_reservation_cnt < m_reservation_depth`, which is a
*flag on the request* (easy: depth 1, conservative: `MAX_RESERVATION_DEPTH`),
not a different number of requests. Conservative therefore has roughly the same
`run_match` count as easy — on the 500-job Gaussian matrix, 28,653 against
23,822, a 1.20x difference that is mostly conservative keeping the queue deep
for longer.

The reservation search is the planner-time loop in `dfu_traverser_t::schedule()`:

```c++
for (t = planner_multi_avail_time_first (p, t, duration, agg.data (), len);
     (t != -1 && rc);
     t = planner_multi_avail_time_next (p)) {
    meta.at = t;
    rc = traverser->select (jobspec.resources, root, meta, x);
    ++sched_iters;
    ++resv_iters;
}
```

Every iteration is a full traversal at one candidate start time, and all of them
happen **inside a single `run_match`**. That is why the cost shows up as event
*duration* and never as event count. `resv_attempts` is `resv_iters` carried out
to the annotation, so a level 2 trace can count reservation work directly:

    # total reservation attempts across the run
    zcat trace.pfw.gz | jq -s '[.[]?|select(.name=="run_match")|.args.resv_attempts//0]|add'

    # the jobs whose reservations are expensive to recompute
    zcat trace.pfw.gz \
      | jq -r 'select(.name=="run_match" and .args.outcome=="reserved")
               | "\(.args.jobid) \(.args.resv_attempts)"'

The counters themselves (`sched_iters`, `resv_iters` in `dfu.cpp`, surfaced
through `perf.tmp_iter_count` and `perf.tmp_resv_iter_count`) are maintained
unconditionally at every level, including level 0 — they are two integer
increments per traversal. Only the *reporting* is gated, at level 2. Nothing
about counting reservation attempts requires the level 4 traversal tier.

`resv_attempts` is 0 for a job that allocated outright, for a plain `allocate`,
and for a satisfiability check. `sched_iters` is 1 for a satisfiability check.
Both are reset at the top of every match, so a failed or rejected match reports
0 rather than the previous match's tally.

### What it shows

500-job Gaussian, `cores` jobspecs, `reservation-depth = 4`, `queue-depth = 1024`,
one run each:

| | easy | conservative | ratio |
|---|---|---|---|
| `run_match` events | 23,827 | 28,718 | 1.21x |
| `cancel_request_cb` | 1,467 | 28,718 | 19.6x |
| `cmd = allocate_orelse_reserve` | 1,159 | 28,718 | 24.8x |
| `outcome = reserved` | 967 | 28,218 | 29.2x |
| **`resv_attempts` total** | **967** | **34,395** | **35.6x** |
| `resv_attempts` max on one match | 1 | 7 | |

The match count barely moves; the reservation work is 35x. Two independent
checks that the counters are wired correctly: reservations plus completions
equals the cancel count in both cells (967 + 500 = 1,467; 28,218 + 500 =
28,718), and allocations split across the two `cmd` values sum to 500 in both.

Note the shape of conservative's cost. Each individual reservation is cheap —
1.22 candidate start times tried on average, never more than 7. What makes
conservative expensive is that it computes **29x as many reservations**, because
every pending job is reserve-eligible and every one of them is torn down and
recomputed on the next scheduling pass.

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
