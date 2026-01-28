/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
