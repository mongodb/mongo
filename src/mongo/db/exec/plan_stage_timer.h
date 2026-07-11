// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/duration.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Returns a ScopedTimer that accumulates elapsed time into `counter` using the clock source
 * appropriate for `precision`:
 *   * kMillis -> the OperationContext's cached fast clock source,
 *   * kNanos  -> the service context's tick source.
 * Returns boost::none when `opCtx` is null or `precision` is kNoTiming.
 */
inline boost::optional<ScopedTimer> maybeMakeScopedTimer(OperationContext* opCtx,
                                                         QueryExecTimerPrecision precision,
                                                         Nanoseconds* counter) {
    if (!opCtx || precision == QueryExecTimerPrecision::kNoTiming) {
        return boost::none;
    }
    if (MONGO_likely(precision == QueryExecTimerPrecision::kMillis)) {
        return boost::optional<ScopedTimer>(
            boost::in_place_init, counter, &opCtx->fastClockSource());
    }
    return boost::optional<ScopedTimer>(
        boost::in_place_init, counter, opCtx->getServiceContext()->getTickSource());
}

/**
 * As above, but accepts a ServiceContext directly. Used by callers that don't have an
 * OperationContext at hand (e.g. the aggregation Stage hierarchy).
 */
inline boost::optional<ScopedTimer> maybeMakeScopedTimer(ServiceContext* serviceCtx,
                                                         QueryExecTimerPrecision precision,
                                                         Nanoseconds* counter) {
    if (!serviceCtx || precision == QueryExecTimerPrecision::kNoTiming) {
        return boost::none;
    }
    if (MONGO_likely(precision == QueryExecTimerPrecision::kMillis)) {
        return boost::optional<ScopedTimer>(
            boost::in_place_init, counter, serviceCtx->getFastClockSource());
    }
    return boost::optional<ScopedTimer>(boost::in_place_init, counter, serviceCtx->getTickSource());
}

}  // namespace mongo
