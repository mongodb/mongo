/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/commands/server_status_metric.h"


namespace mongo {
struct CursorStats {
private:
    CursorStats() = default;
    ~CursorStats() = default;

    // Delete the copy and move constructors
    CursorStats(const CursorStats&) = delete;
    CursorStats& operator=(const CursorStats&) = delete;
    CursorStats(CursorStats&&) = delete;
    CursorStats& operator=(CursorStats&&) = delete;

public:
    CounterMetric cursorStatsOpen{"cursor.open.total"};
    CounterMetric cursorStatsOpenPinned{"cursor.open.pinned"};
    CounterMetric cursorStatsOpenNoTimeout{"cursor.open.noTimeout"};
    CounterMetric cursorStatsTimedOut{"cursor.open.timedOut"};
    CounterMetric cursorStatsTotalOpened{"cursor.totalOpened"};
    CounterMetric cursorStatsMoreThanOneBatch{"cursor.moreThanOneBatch"};

    CounterMetric cursorStatsMultiTarget{"cursor.open.multiTarget"};
    CounterMetric cursorStatsSingleTarget{"cursor.open.singleTarget"};
    CounterMetric cursorStatsQueuedData{"cursor.open.queuedData"};

    CounterMetric cursorStatsLifespanLessThan1Second{"cursor.lifespan.lessThan1Second"};
    CounterMetric cursorStatsLifespanLessThan5Seconds{"cursor.lifespan.lessThan5Seconds"};
    CounterMetric cursorStatsLifespanLessThan15Seconds{"cursor.lifespan.lessThan15Seconds"};
    CounterMetric cursorStatsLifespanLessThan30Seconds{"cursor.lifespan.lessThan30Seconds"};
    CounterMetric cursorStatsLifespanLessThan1Minute{"cursor.lifespan.lessThan1Minute"};
    CounterMetric cursorStatsLifespanLessThan10Minutes{"cursor.lifespan.lessThan10Minutes"};
    CounterMetric cursorStatsLifespanGreaterThanOrEqual10Minutes{
        "cursor.lifespan.greaterThanOrEqual10Minutes"};

    static CursorStats& getInstance() {
        static CursorStats cursorStats;
        return cursorStats;
    }

    void reset() {
        cursorStatsOpen.decrement(cursorStatsOpen.get());
        cursorStatsOpenPinned.decrement(cursorStatsOpenPinned.get());
        cursorStatsMultiTarget.decrement(cursorStatsMultiTarget.get());
        cursorStatsSingleTarget.decrement(cursorStatsSingleTarget.get());
        cursorStatsQueuedData.decrement(cursorStatsQueuedData.get());
    }
};

}  // namespace mongo
