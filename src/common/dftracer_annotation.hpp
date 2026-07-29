/*****************************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, LICENSE)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\*****************************************************************************/

/*
 * dftracer_annotation.hpp - depth-gated dftracer annotations
 *
 * Every dftracer annotation in this tree is tagged with the tier of the
 * scheduler it sits in, and DFTRACER_ANNOTATION_LEVEL (a CMake cache variable,
 * -DDFTRACER_ANNOTATION_LEVEL=N) selects how deep to instrument. Levels are
 * cumulative: each one keeps everything above it and adds its own tier.
 *
 *   0  OFF        no annotations at all, and no dependency on dftracer
 *   1  QMANAGER   queue manager: the entry point, where jobs arrive and
 *                 allocations are handed back
 *   2  MATCH      the resource module's request path -- match, match_multi,
 *                 cancel, feasibility, and the rest of the RPC callbacks
 *   3  GRAPH      graph construction and policy selection: readers, the graph
 *                 store, match-policy factories, jobspec parsing
 *   4  TRAVERSE   the traversal itself, plus the standalone Fluxion utilities
 *   5  PLANNER    planner and string-interner leaf calls
 *
 * Level is a cost control. Measured on the rabbit300 emulation (300 jobs, 1153
 * nodes, 2026-07-29), against a level 0 control on the same node and tree:
 *
 *     level 0                          62 s   1.00x         - events
 *     level 2, DFTRACER_DISABLE_IO=1   64 s   1.03x     1,536 events
 *     level 2                         128 s   2.06x     1,536 events
 *     level 5, DFTRACER_DISABLE_IO=1  155 s   2.50x  2,570,814 events
 *     level 5                         351 s   5.66x  2,570,814 events + 8 POSIX
 *
 * Note what that says: most of the cost is not the annotations, it is the gotcha
 * wrappers dftracer installs around POSIX/stdio as soon as the logger comes up.
 * Those two level 5 rows record exactly the same annotations -- the entire
 * difference in content is 8 POSIX/execv events, for 196 seconds, because
 * dftracer only records I/O under DFTRACER_DATA_DIR and every other syscall in
 * the broker pays for a wrapper that declines to record it. Set
 * DFTRACER_DISABLE_IO=1 unless you want I/O events; level 2 is then free.
 *
 * Level 5 stays expensive regardless, at 2.57M events with 74% of them from the
 * interner's get_both(). Per-string and per-vertex annotations are far costlier
 * under flux-fiction than they would be in a normal run: it drives the broker
 * under libfaketime with FAKETIME_NO_CACHE=1, so each clock read a DFTracer
 * constructor and destructor performs becomes an open/read/close of the stamp
 * file on /dev/shm (~20 us) instead of a vDSO call (~0.02 us). Levels 1-4 leave
 * those paths alone.
 *
 * Usage -- name the tier, do not test the level directly:
 *
 *     static int run_match (...)
 *     {
 *         FLUX_DFT_FN (MATCH);
 *         FLUX_DFT_UPDATE (MATCH, "comp", "cpu");
 *         ...
 *     }
 *
 * Below its level a tier expands to nothing, so annotated functions compile
 * unchanged and no dftracer symbol is referenced.
 */

#ifndef DFTRACER_ANNOTATION_HPP
#define DFTRACER_ANNOTATION_HPP

#ifndef DFTRACER_ANNOTATION_LEVEL
#define DFTRACER_ANNOTATION_LEVEL 0
#endif

#define FLUX_DFT_LEVEL_OFF 0
#define FLUX_DFT_LEVEL_QMANAGER 1
#define FLUX_DFT_LEVEL_MATCH 2
#define FLUX_DFT_LEVEL_GRAPH 3
#define FLUX_DFT_LEVEL_TRAVERSE 4
#define FLUX_DFT_LEVEL_PLANNER 5

#if DFTRACER_ANNOTATION_LEVEL > FLUX_DFT_LEVEL_OFF
#include <dftracer/dftracer.h>
#endif

/* Tier dispatch. FLUX_DFT_FN (MATCH) pastes to FLUX_DFT_FN_MATCH (), which is
 * defined below as either the real annotation or nothing, depending on the
 * level this tree was configured with.
 */
#define FLUX_DFT_FN(tier) FLUX_DFT_FN_##tier ()
#define FLUX_DFT_UPDATE(tier, key, val) FLUX_DFT_UPDATE_##tier (key, val)

/* Each tier is enabled when the configured level reaches it. The empty forms
 * are `((void)0)` rather than blank so that a stray semicolon after the macro
 * stays a valid statement wherever it appears.
 */
#if DFTRACER_ANNOTATION_LEVEL >= FLUX_DFT_LEVEL_QMANAGER
#define FLUX_DFT_FN_QMANAGER() DFTRACER_CPP_FUNCTION ()
#define FLUX_DFT_UPDATE_QMANAGER(key, val) DFTRACER_CPP_FUNCTION_UPDATE (key, val)
#else
#define FLUX_DFT_FN_QMANAGER() ((void)0)
#define FLUX_DFT_UPDATE_QMANAGER(key, val) ((void)0)
#endif

#if DFTRACER_ANNOTATION_LEVEL >= FLUX_DFT_LEVEL_MATCH
#define FLUX_DFT_FN_MATCH() DFTRACER_CPP_FUNCTION ()
#define FLUX_DFT_UPDATE_MATCH(key, val) DFTRACER_CPP_FUNCTION_UPDATE (key, val)
#else
#define FLUX_DFT_FN_MATCH() ((void)0)
#define FLUX_DFT_UPDATE_MATCH(key, val) ((void)0)
#endif

#if DFTRACER_ANNOTATION_LEVEL >= FLUX_DFT_LEVEL_GRAPH
#define FLUX_DFT_FN_GRAPH() DFTRACER_CPP_FUNCTION ()
#define FLUX_DFT_UPDATE_GRAPH(key, val) DFTRACER_CPP_FUNCTION_UPDATE (key, val)
#else
#define FLUX_DFT_FN_GRAPH() ((void)0)
#define FLUX_DFT_UPDATE_GRAPH(key, val) ((void)0)
#endif

#if DFTRACER_ANNOTATION_LEVEL >= FLUX_DFT_LEVEL_TRAVERSE
#define FLUX_DFT_FN_TRAVERSE() DFTRACER_CPP_FUNCTION ()
#define FLUX_DFT_UPDATE_TRAVERSE(key, val) DFTRACER_CPP_FUNCTION_UPDATE (key, val)
#else
#define FLUX_DFT_FN_TRAVERSE() ((void)0)
#define FLUX_DFT_UPDATE_TRAVERSE(key, val) ((void)0)
#endif

#if DFTRACER_ANNOTATION_LEVEL >= FLUX_DFT_LEVEL_PLANNER
#define FLUX_DFT_FN_PLANNER() DFTRACER_CPP_FUNCTION ()
#define FLUX_DFT_UPDATE_PLANNER(key, val) DFTRACER_CPP_FUNCTION_UPDATE (key, val)
#else
#define FLUX_DFT_FN_PLANNER() ((void)0)
#define FLUX_DFT_UPDATE_PLANNER(key, val) ((void)0)
#endif

/* Process-level setup for the standalone utilities (resource-query, grug2dot,
 * flux-jobspec-validate): call this once at the top of main().
 *
 * It registers the finalize as well as doing the init, because dftracer only
 * writes its buffered events out on an explicit finalize() -- init alone leaves
 * a 0-byte trace file behind, which is what the annotations as generated would
 * have produced. atexit is the right hook for a standalone binary, and covers
 * exit() as well as returning from main.
 *
 * The long-lived broker modules must NOT use this: they are dlclose()d and
 * reloaded mid-run by flux-fiction, and glibc runs a DSO's atexit handlers at
 * dlclose. See dftracer_bootstrap.hpp.
 */
#if DFTRACER_ANNOTATION_LEVEL > FLUX_DFT_LEVEL_OFF

#include <cstdlib>

namespace Flux {
namespace dftracer_annotation {

inline void process_init ()
{
    DFTRACER_C_INIT (NULL, NULL, NULL);
    std::atexit ([] () { DFTRACER_C_FINI (); });
}

}  // namespace dftracer_annotation
}  // namespace Flux

#define FLUX_DFT_PROCESS_INIT() Flux::dftracer_annotation::process_init ()
#else
#define FLUX_DFT_PROCESS_INIT() ((void)0)
#endif

#endif  // DFTRACER_ANNOTATION_HPP

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
