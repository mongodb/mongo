// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/clock_source_mock.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ClockSourceMockTest, ClockSourceShouldReportThatItIsNotSystemClock) {
    ClockSourceMock cs;
    ASSERT(!cs.tracksSystemClock());
}

TEST(ClockSourceMockTest, ExpiredAlarmExecutesWhenSet) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const Date_t alarmDate = cs.now();
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(alarmDate, alarmAction);
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
    alarmFiredCount = 0;
    cs.advance(Seconds{1});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.setAlarm(alarmDate, alarmAction);
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, AlarmExecutesAfterExpirationUsingAdvance) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const Date_t alarmDate = cs.now() + Seconds{10};
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(alarmDate, alarmAction);
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{8});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{1});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{20});
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
    cs.advance(Seconds{1});
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, AlarmExecutesAfterExpirationUsingReset) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const Date_t startDate = cs.now();
    const Date_t alarmDate = startDate + Seconds{10};
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(alarmDate, alarmAction);
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.reset(startDate + Seconds{8});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.reset(startDate + Seconds{9});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.reset(startDate + Seconds{20});
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
    cs.reset(startDate + Seconds{21});
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, MultipleAlarmsWithSameDeadlineTriggeredAtSameTime) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const Date_t alarmDate = cs.now() + Seconds{10};
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(alarmDate, alarmAction);
    cs.setAlarm(alarmDate, alarmAction);
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{20});
    ASSERT_EQ(2, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, MultipleAlarmsWithDifferentDeadlineTriggeredAtSameTime) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(cs.now() + Seconds{1}, alarmAction);
    cs.setAlarm(cs.now() + Seconds{10}, alarmAction);
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{20});
    ASSERT_EQ(2, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, MultipleAlarmsWithDifferentDeadlineTriggeredAtDifferentTimes) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const auto alarmAction = [&] {
        ++alarmFiredCount;
    };
    cs.setAlarm(cs.now() + Seconds{1}, alarmAction);
    cs.setAlarm(cs.now() + Seconds{10}, alarmAction);
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{5});
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
    cs.advance(Seconds{5});
    ASSERT_EQ(2, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, AlarmScheudlesExpiredAlarmWhenSignaled) {
    ClockSourceMock cs;
    const auto beginning = cs.now();
    int alarmFiredCount = 0;
    cs.setAlarm(beginning + Seconds{1}, [&] {
        ++alarmFiredCount;
        cs.setAlarm(beginning, [&] { ++alarmFiredCount; });
    });
    ASSERT_EQ(0, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(2, alarmFiredCount);
}

TEST(ClockSourceMockTest, ExpiredAlarmScheudlesExpiredAlarm) {
    ClockSourceMock cs;
    const auto beginning = cs.now();
    int alarmFiredCount = 0;
    cs.setAlarm(beginning, [&] {
        ++alarmFiredCount;
        cs.setAlarm(beginning, [&] { ++alarmFiredCount; });
    });
    ASSERT_EQ(2, alarmFiredCount);
}

TEST(ClockSourceMockTest, AlarmScheudlesAlarmWhenSignaled) {
    ClockSourceMock cs;
    const auto beginning = cs.now();
    int alarmFiredCount = 0;
    cs.setAlarm(beginning + Seconds{1}, [&] {
        ++alarmFiredCount;
        cs.setAlarm(beginning + Seconds{2}, [&] { ++alarmFiredCount; });
    });
    ASSERT_EQ(0, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(1, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(2, alarmFiredCount);
}
}  // namespace
}  // namespace mongo
