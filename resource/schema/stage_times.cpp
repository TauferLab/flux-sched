/*****************************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, LICENSE)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\*****************************************************************************/

extern "C" {
#if HAVE_CONFIG_H
#include <config.h>
#endif
}

#include <ostream>
#include <string>

#include "resource/schema/stage_times.hpp"

namespace Flux {
namespace resource_model {

stage_times_t stage_times;

void print_stage_times (std::ostream &out)
{
    out << "INFO:"
        << " STAGE"
        << " PARSE=" << std::to_string (stage_times.parse)
        << " PRIME=" << std::to_string (stage_times.prime)
        << " MATCH=" << std::to_string (stage_times.match)
        << " UPDATE=" << std::to_string (stage_times.update)
        << " EMIT=" << std::to_string (stage_times.emit)
        << " TOTAL=" << std::to_string (stage_times.total) << std::endl;
}

}  // namespace resource_model
}  // namespace Flux

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
