// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_severity_suppressor.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <ratio>

namespace mongo::logv2 {
namespace {

TEST(LogSeveritySuppressorTest, SuppressorWorksCorrectly) {
    LogSeverity normalSeverity = LogSeverity::Info();
    LogSeverity quietSeverity = LogSeverity::Debug(2);
    int quiesceMs = 1000;
    Milliseconds quiescePeriod{quiesceMs};

    ClockSourceMock clockSource;

    for (int i = 0; i < quiesceMs; ++i) {
        SeveritySuppressor suppressor(&clockSource, quiescePeriod, normalSeverity, quietSeverity);
        ASSERT_EQ(suppressor(), normalSeverity);
        clockSource.advance(Milliseconds{i});
        ASSERT_EQ(suppressor(), quietSeverity);
        ASSERT_EQ(suppressor(), quietSeverity);
    }

    SeveritySuppressor suppressor(&clockSource, quiescePeriod, normalSeverity, quietSeverity);
    ASSERT_EQ(suppressor(), normalSeverity);

    clockSource.advance(quiescePeriod);
    ASSERT_EQ(suppressor(), normalSeverity);

    clockSource.advance(quiescePeriod * 2);
    ASSERT_EQ(suppressor(), normalSeverity);
}

TEST(LogSeveritySuppressorTest, RateIncrease) {
    LogSeverity normalSeverity = LogSeverity::Info();
    LogSeverity quietSeverity = LogSeverity::Debug(2);
    int quiesceMs = 1000;
    Milliseconds quiescePeriod{quiesceMs};

    ClockSourceMock clockSource;

    SeveritySuppressor suppressor(&clockSource, quiescePeriod, normalSeverity, quietSeverity);
    ASSERT_EQ(suppressor(), normalSeverity);

    clockSource.advance(quiescePeriod / 2);
    ASSERT_EQ(suppressor(), quietSeverity);

    suppressor.setPeriod(quiescePeriod / 10);
    ASSERT_EQ(suppressor(), normalSeverity);
    ASSERT_EQ(suppressor(), quietSeverity);
}

TEST(LogSeveritySuppressorTest, BackwardsClockMovementRetainsQuietness) {
    LogSeverity normalSeverity = LogSeverity::Info();
    LogSeverity quietSeverity = LogSeverity::Debug(2);
    int quiesceMs = 1000;
    Milliseconds quiescePeriod{quiesceMs};

    Date_t timeOne = Date_t::fromMillisSinceEpoch(0);
    Date_t timeTwo = Date_t::fromMillisSinceEpoch(quiesceMs);
    Date_t timeThree = Date_t::fromMillisSinceEpoch(quiesceMs + (quiesceMs / 2));

    ClockSourceMock clockSource;
    clockSource.reset(timeTwo);

    SeveritySuppressor suppressor(&clockSource, quiescePeriod, normalSeverity, quietSeverity);
    ASSERT_EQ(suppressor(), normalSeverity);
    ASSERT_EQ(suppressor(), quietSeverity);

    clockSource.reset(timeThree);
    ASSERT_EQ(suppressor(), quietSeverity);

    clockSource.reset(timeTwo);
    ASSERT_EQ(suppressor(), quietSeverity);

    clockSource.reset(timeOne);
    ASSERT_EQ(suppressor(), quietSeverity);

    clockSource.reset(timeThree);
    ASSERT_EQ(suppressor(), quietSeverity);
}

}  // namespace

}  // namespace mongo::logv2
