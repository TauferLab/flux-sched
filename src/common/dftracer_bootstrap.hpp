/*****************************************************************************\
 * dftracer_bootstrap.hpp - get the dftracer trace out of flux-broker
 *
 * The auto-injected DFTRACER_CPP_FUNCTION() annotations in the Fluxion modules
 * run inside flux-broker, which dlopen()s sched-fluxion-{resource,qmanager,
 * feasibility}.so and runs each mod_main() on its own thread. Nothing in that
 * path ever calls dftracer's initialize/finalize, and both matter:
 *
 *   - initialize is not required, and is best left alone: the DFTracer object
 *     constructed by DFTRACER_CPP_FUNCTION() brings the logger up on first use.
 *   - finalize IS required: without it the trace file is created but stays
 *     0 bytes. The buffered events are only written out by finalize().
 *
 * The awkward part is *when* to finalize. flux-fiction does not load Fluxion
 * once: it loads it, then unloads and reloads it a few simulated seconds in,
 * once it has the emulated resource set configured. Observed on rabbit300:
 *
 *     23:59:32.215  insmod sched-fluxion-resource        <- generation 0
 *     23:59:37.506  module sched-fluxion-resource exited
 *     23:59:41.869  insmod sched-fluxion-resource        <- generation 1
 *     00:06:05.541  module sched-fluxion-resource exited
 *
 * glibc runs a DSO's atexit handlers when that DSO is dlclose()d, not only at
 * process exit, so a single atexit(finalize) fires at the FIRST unload -- and
 * everything after it, i.e. the entire run, is lost. That is exactly what a
 * first attempt produced: a well-formed trace that ends at 23:59:37.506.
 *
 * Giving each generation its own trace file does not work: dftracer's logger is
 * a process-global singleton that takes its output path from the environment at
 * first use, and once finalized it does not come back -- generation 1 with an
 * explicit per-generation filename produced no output at all.
 *
 * What does work is to leave the logger alone until the load that matters.
 * Events accumulate across the unload (libdftracer_core is not itself unloaded),
 * so registering the handler only from generation 1 onward yields one file
 * covering both generations, flushed at 00:06:05 when the run is over.
 * DFTRACER_FLUXION_FINALIZE_GENERATION overrides which load arms it, for a
 * caller whose reload pattern differs.
 *
 * There is deliberately no initialize call: the DFTracer object built by
 * DFTRACER_CPP_FUNCTION() brings the logger up on first use, and initialize is
 * the half of the API that is unsafe here -- paired initialize/finalize calls
 * from concurrent threads segfault (dftracer 2.1.0 / libdftracer_core 4.2.0,
 * measured 2026-07-29). For the same reason this must be called from exactly
 * ONE module. sched-fluxion-resource is the choice: flux-fiction always loads
 * it, and in both generations above it is the last of the three to be unloaded,
 * so its finalize flushes after the others have stopped emitting. The logger is
 * process-global, so that one finalize captures every module's events, not only
 * this module's.
\*****************************************************************************/

#ifndef DFTRACER_BOOTSTRAP_HPP
#define DFTRACER_BOOTSTRAP_HPP

#include "src/common/dftracer_annotation.hpp"

#if DFTRACER_ANNOTATION_LEVEL > FLUX_DFT_LEVEL_OFF

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Flux {
namespace dftracer_bootstrap {

namespace detail {

constexpr const char *GEN_ENV = "DFTRACER_FLUXION_GENERATION";
constexpr const char *FINALIZE_GEN_ENV = "DFTRACER_FLUXION_FINALIZE_GENERATION";
constexpr long DEFAULT_FINALIZE_GEN = 1;

inline std::once_flag &once_flag ()
{
    static std::once_flag flag;
    return flag;
}

inline void finalize_at_unload ()
{
    DFTRACER_C_FINI ();
}

}  // namespace detail

/*! Start a dftracer session for this load of the module, and arrange for it to
 *  be flushed when the module is unloaded.
 *
 *  No-op unless DFTRACER_ENABLE=1. Safe to call repeatedly within one module
 *  load; must not be called from more than one module.
 */
inline void begin_session ()
{
    const char *enable = std::getenv ("DFTRACER_ENABLE");
    if (!enable || enable[0] != '1')
        return;

    std::call_once (detail::once_flag (), [] {
        // Which load of the module is this? Statics are reset by the dlclose,
        // so the counter has to live somewhere that outlives the module: the
        // process environment.
        const char *gen_s = std::getenv (detail::GEN_ENV);
        long gen = gen_s ? std::strtol (gen_s, nullptr, 10) : 0;
        if (gen < 0)
            gen = 0;

        char next[32];
        std::snprintf (next, sizeof (next), "%ld", gen + 1);
        setenv (detail::GEN_ENV, next, 1);

        const char *want_s = std::getenv (detail::FINALIZE_GEN_ENV);
        long want = want_s ? std::strtol (want_s, nullptr, 10) : detail::DEFAULT_FINALIZE_GEN;

        // Arming earlier would flush -- and permanently close -- the trace at
        // the configure-time unload, discarding the whole run that follows.
        if (gen < want)
            return;

        std::atexit (detail::finalize_at_unload);
    });
}

}  // namespace dftracer_bootstrap
}  // namespace Flux

#else  // DFTRACER_ANNOTATION_LEVEL == 0

namespace Flux {
namespace dftracer_bootstrap {
inline void begin_session ()
{
}
}  // namespace dftracer_bootstrap
}  // namespace Flux

#endif  // DFTRACER_ANNOTATION_LEVEL

#endif  // DFTRACER_BOOTSTRAP_HPP

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
