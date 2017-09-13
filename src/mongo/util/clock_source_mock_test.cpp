/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

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
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
    alarmFiredCount = 0;
    cs.advance(Seconds{1});
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
    ASSERT_EQ(1, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, AlarmExecutesAfterExpirationUsingAdvance) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const Date_t alarmDate = cs.now() + Seconds{10};
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
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
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
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
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
    ASSERT_OK(cs.setAlarm(alarmDate, alarmAction));
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{20});
    ASSERT_EQ(2, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, MultipleAlarmsWithDifferentDeadlineTriggeredAtSameTime) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(cs.now() + Seconds{1}, alarmAction));
    ASSERT_OK(cs.setAlarm(cs.now() + Seconds{10}, alarmAction));
    ASSERT_EQ(0, alarmFiredCount) << cs.now();
    cs.advance(Seconds{20});
    ASSERT_EQ(2, alarmFiredCount) << cs.now();
}

TEST(ClockSourceMockTest, MultipleAlarmsWithDifferentDeadlineTriggeredAtDifferentTimes) {
    ClockSourceMock cs;
    int alarmFiredCount = 0;
    const auto alarmAction = [&] { ++alarmFiredCount; };
    ASSERT_OK(cs.setAlarm(cs.now() + Seconds{1}, alarmAction));
    ASSERT_OK(cs.setAlarm(cs.now() + Seconds{10}, alarmAction));
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
    ASSERT_OK(cs.setAlarm(beginning + Seconds{1},
                          [&] {
                              ++alarmFiredCount;
                              ASSERT_OK(cs.setAlarm(beginning, [&] { ++alarmFiredCount; }));
                          }));
    ASSERT_EQ(0, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(2, alarmFiredCount);
}

TEST(ClockSourceMockTest, ExpiredAlarmScheudlesExpiredAlarm) {
    ClockSourceMock cs;
    const auto beginning = cs.now();
    int alarmFiredCount = 0;
    ASSERT_OK(cs.setAlarm(beginning, [&] {
        ++alarmFiredCount;
        ASSERT_OK(cs.setAlarm(beginning, [&] { ++alarmFiredCount; }));
    }));
    ASSERT_EQ(2, alarmFiredCount);
}

TEST(ClockSourceMockTest, AlarmScheudlesAlarmWhenSignaled) {
    ClockSourceMock cs;
    const auto beginning = cs.now();
    int alarmFiredCount = 0;
    ASSERT_OK(cs.setAlarm(beginning + Seconds{1},
                          [&] {
                              ++alarmFiredCount;
                              ASSERT_OK(
                                  cs.setAlarm(beginning + Seconds{2}, [&] { ++alarmFiredCount; }));
                          }));
    ASSERT_EQ(0, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(1, alarmFiredCount);
    cs.advance(Seconds{1});
    ASSERT_EQ(2, alarmFiredCount);
}
}
}  // namespace mongo
