// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"

namespace mongo {

/**
 * Increments a DurationCounter when this object goes out of scope, using the TickSource available
 * through the opCtx to track the passage of time.
 */
template <typename TimeUnit>
class ScopedTimerMetric {
public:
    ScopedTimerMetric(TickSource* tickSource, DurationCounter64<TimeUnit>& counter)
        : _tickSource(tickSource),
          _startTicks(_tickSource->ticksTo<TimeUnit>(_tickSource->getTicks())),
          _counter(counter) {}

    ScopedTimerMetric(OperationContext* opCtx, DurationCounter64<TimeUnit>& counter)
        : ScopedTimerMetric(opCtx->getServiceContext()->getTickSource(), counter) {}

    ~ScopedTimerMetric() {
        _counter.increment(_tickSource->ticksTo<TimeUnit>(_tickSource->getTicks()) - _startTicks);
    }

private:
    TickSource* _tickSource;
    TimeUnit _startTicks;
    DurationCounter64<TimeUnit>& _counter;
};

}  // namespace mongo
