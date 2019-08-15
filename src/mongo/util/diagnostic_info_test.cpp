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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/util/diagnostic_info.h"

#include <string>

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"

namespace mongo {
TEST(DiagnosticInfo, BasicSingleThread) {
    // set up serviceContext and clock source
    auto serviceContext = ServiceContext::make();
    auto clockSource = std::make_unique<ClockSourceMock>();
    auto clockSourcePointer = clockSource.get();
    serviceContext->setFastClockSource(std::move(clockSource));
    setGlobalServiceContext(std::move(serviceContext));

    // take the initial diagnostic info
    DiagnosticInfo capture1 = takeDiagnosticInfo("capture1"_sd);
    ASSERT_EQ(capture1.getCaptureName(), "capture1");

    // mock time advancing and check that the current time is greater than capture1's timestamp
    clockSourcePointer->advance(Seconds(1));
    ASSERT_LT(capture1.getTimestamp(), clockSourcePointer->now());

    // take a second diagnostic capture and compare its fields to the first
    DiagnosticInfo capture2 = takeDiagnosticInfo("capture2"_sd);
    ASSERT_LT(capture1.getTimestamp(), capture2.getTimestamp());
    ASSERT_EQ(capture2.getCaptureName(), "capture2");
    ASSERT_NE(capture2, capture1);

    clockSourcePointer->advance(Seconds(1));
    ASSERT_LT(capture2.getTimestamp(), clockSourcePointer->now());
}

using MaybeDiagnosticInfo = boost::optional<DiagnosticInfo>;
void recurseAndCaptureInfo(MaybeDiagnosticInfo& info, size_t i);

TEST(DiagnosticInfo, StackTraceTest) {
    // set up serviceContext and clock source
    auto serviceContext = ServiceContext::make();
    auto clockSource = std::make_unique<ClockSourceMock>();
    serviceContext->setFastClockSource(std::move(clockSource));
    setGlobalServiceContext(std::move(serviceContext));

    MaybeDiagnosticInfo infoRecurse0;
    recurseAndCaptureInfo(infoRecurse0, 0);
    ASSERT(infoRecurse0);
    log() << *infoRecurse0;
    auto trace0 = infoRecurse0->makeStackTrace();
    log() << trace0;

#ifdef __linux__
    auto testRecursion = [&](size_t i, const MaybeDiagnosticInfo& infoRecurse) {
        ASSERT(infoRecurse);
        log() << *infoRecurse;

        auto trace = infoRecurse->makeStackTrace();
        log() << trace;
        ASSERT_EQ(trace0.frames.size() + i, trace.frames.size());

        auto it = trace.frames.begin();
        auto it0 = trace0.frames.begin();

        for (; *it == *it0; ++it, ++it0) {
            // The begining of the trace should be the same
        }

        size_t j = 0;
        auto recursiveFrame = *it;
        for (; *it == recursiveFrame; ++it, ++j) {
            // Advance the recursive trace through the recursion
            ASSERT_NE(*it, *it0);

            // The recursive frame should always be on the main executable
            ASSERT_EQ(it->objectPath, "");
        }
        ASSERT_EQ(j, i);

        // The frame right above the recursion will be a different return
        ASSERT_NE(*it, *it0);
        ++it, ++it0;

        for (; it0 != trace0.frames.end() && *it == *it0; ++it, ++it0) {
            // The end of the trace should be the same
        }

        ASSERT(it == trace.frames.end());
    };

    {
        volatile size_t i = 3;  // NOLINT
        MaybeDiagnosticInfo infoRecurse;
        recurseAndCaptureInfo(infoRecurse, i);
        testRecursion(i, infoRecurse);
    }

    {
        volatile size_t i = 10;  // NOLINT
        MaybeDiagnosticInfo infoRecurse;
        recurseAndCaptureInfo(infoRecurse, i);
        testRecursion(i, infoRecurse);
    }
#else
    ASSERT_TRUE(trace0.frames.empty());
#endif
}

MONGO_COMPILER_NOINLINE void recurseAndCaptureInfo(MaybeDiagnosticInfo& info, size_t i) {
    // Prevent tail-call optimization.
#ifndef _WIN32
    asm volatile("");  // NOLINT
#endif

    if (i == 0) {
        info = takeDiagnosticInfo("Recursion!"_sd);
        return;
    }

    recurseAndCaptureInfo(info, --i);
}
}  // namespace mongo
