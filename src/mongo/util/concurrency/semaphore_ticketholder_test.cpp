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

#include "mongo/util/concurrency/semaphore_ticketholder.h"

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/ticketholder_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

namespace {
using namespace mongo;

class SemaphoreTicketHolderTest : public TicketHolderTestFixture {};

TEST_F(SemaphoreTicketHolderTest, BasicTimeoutSemaphore) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    basicTimeout(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(&serviceContext, 1, false /* trackPeakUsed */));
}

TEST_F(SemaphoreTicketHolderTest, ResizeStatsSemaphore) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());

    resizeTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(&serviceContext, 1, false /* trackPeakUsed */),
        tickSource);
}

TEST_F(SemaphoreTicketHolderTest, Interruption) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    interruptTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(&serviceContext, 1, false /* trackPeakUsed */));
}

TEST_F(SemaphoreTicketHolderTest, PriorityBookkeeping) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    priorityBookkeepingTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(&serviceContext, 1, false /* trackPeakUsed */),
        AdmissionContext::Priority::kExempt,
        [](auto statsWhileProcessing, auto statsWhenFinished) {
            ASSERT_EQ(statsWhileProcessing.getObjectField("normalPriority")
                          .getIntField("startedProcessing"),
                      0);
            ASSERT_EQ(
                statsWhileProcessing.getObjectField("exempt").getIntField("startedProcessing"), 1);
            ASSERT_EQ(statsWhenFinished.getObjectField("normalPriority")
                          .getIntField("finishedProcessing"),
                      0);
            ASSERT_EQ(statsWhenFinished.getObjectField("exempt").getIntField("finishedProcessing"),
                      1);
        });
}

}  // namespace
