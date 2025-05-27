/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/logv2/log_severity_suppressor.h"

#include "mongo/base/string_data.h"
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
