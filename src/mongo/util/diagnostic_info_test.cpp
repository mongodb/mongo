/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/util/diagnostic_info.h"

#include <string>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
TEST(DiagnosticInfo, BasicSingleThread) {
    // set up serviceContext and clock source
    auto serviceContext = ServiceContext::make();
    auto clockSource = std::make_unique<ClockSourceMock>();
    auto clockSourcePointer = clockSource.get();
    serviceContext->setFastClockSource(std::move(clockSource));
    setGlobalServiceContext(std::move(serviceContext));

    // take the initial diagnostic info
    DiagnosticInfo capture1 = DiagnosticInfo::capture("capture1"_sd);
    ASSERT_EQ(capture1.getCaptureName(), "capture1");

    // mock time advancing and check that the current time is greater than capture1's timestamp
    clockSourcePointer->advance(Seconds(1));
    ASSERT_LT(capture1.getTimestamp(), clockSourcePointer->now());

    // take a second diagnostic capture and compare its fields to the first
    DiagnosticInfo capture2 = DiagnosticInfo::capture("capture2"_sd);
    ASSERT_LT(capture1.getTimestamp(), capture2.getTimestamp());
    ASSERT_EQ(capture2.getCaptureName(), "capture2");
    ASSERT_NE(capture2, capture1);

    clockSourcePointer->advance(Seconds(1));
    ASSERT_LT(capture2.getTimestamp(), clockSourcePointer->now());
}
}  // namespace mongo
