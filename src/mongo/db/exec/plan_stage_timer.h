/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
