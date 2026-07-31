/*****************************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, LICENSE)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\*****************************************************************************/

#ifndef STAGE_TIMES_HPP
#define STAGE_TIMES_HPP

#include <chrono>
#include <iosfwd>

/* Per-stage wall times for the most recent match_allocate () call.  The
 * existing perf_stats timers cover the whole call as a single number; these
 * break it down so jobspec parsing and the graph update can be told apart
 * from the traversal itself.
 *
 * This deliberately lives outside perf_data.hpp.  That header defines a
 * match_perf_t which collides with an unrelated struct of the same name in
 * resource/utilities/command.hpp; the two never share a translation unit
 * today, and resource-query pulls in command.hpp.  Keeping the stage timers
 * self-contained lets the traverser, the reapi CLI binding and the command
 * layer all include them without tripping that collision.
 */

namespace Flux {
namespace resource_model {

struct stage_times_t {
    void reset ()
    {
        parse = prime = match = update = emit = total = 0.0;
    }

    double parse = 0.0;  /* Jobspec (RFC 14) parse */
    double prime = 0.0;  /* prime_jobspec () + jobmeta build */
    double match = 0.0;  /* schedule () -- the traversal proper */
    double update = 0.0; /* traverser update () -- planner writes */
    double emit = 0.0;   /* match writer emit () to R */
    double total = 0.0;  /* whole match_allocate (), == ELAPSE */
};

/* Monotonic interval timer.  steady_clock rather than gettimeofday so the
 * stage times cannot be perturbed by wall-clock adjustment.
 */
struct stage_timer_t {
    stage_timer_t () : m_t0 (std::chrono::steady_clock::now ())
    {
    }

    double elapsed () const
    {
        return std::chrono::duration<double> (std::chrono::steady_clock::now () - m_t0).count ();
    }

   private:
    std::chrono::time_point<std::chrono::steady_clock> m_t0;
};

extern stage_times_t stage_times;

/* Emit the breakdown on one line so a sweep driver can parse it alongside
 * ELAPSE.  parse+prime+match+update+emit accounts for TOTAL; any shortfall is
 * call overhead outside the timed stages.
 */
void print_stage_times (std::ostream &out);

}  // namespace resource_model
}  // namespace Flux

#endif  // STAGE_TIMES_HPP

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
