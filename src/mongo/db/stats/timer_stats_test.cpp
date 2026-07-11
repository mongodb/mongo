// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/timer_stats.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace {

using namespace mongo;

TEST(TimerStatsTest, GetReportNoRecording) {
    ASSERT_BSONOBJ_EQ(BSON("num" << 0 << "totalMillis" << 0), TimerStats().getReport());
}

TEST(TimerStatsTest, GetReportOneRecording) {
    TimerStats timerStats;
    Timer timer;
    int millis = timerStats.record(timer);
    ASSERT_BSONOBJ_EQ(BSON("num" << 1 << "totalMillis" << millis), timerStats.getReport());
}

}  // namespace
