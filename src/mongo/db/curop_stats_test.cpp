/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/curop.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/tick_source_mock.h"
namespace mongo {

namespace {

class CurOpStatsTest : public ServiceContextTest {};

TEST_F(CurOpStatsTest, CheckWorkingMillisValue) {
    auto opCtx = makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    Milliseconds executionTime = Milliseconds(16);
    Milliseconds waitForTickets = Milliseconds(2);
    Milliseconds waitForFlowControlTicket = Milliseconds(4);

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{100});
    curop->setTickSource_forTest(tickSourceMock.get());

    // Check that execution time advances as expected
    curop->ensureStarted();
    tickSourceMock->advance(executionTime);
    curop->done();
    ASSERT_EQ(duration_cast<Milliseconds>(curop->elapsedTimeExcludingPauses()), executionTime);

    // Check that workingTimeMillis correctly accounts for ticket wait time
    auto locker = shard_role_details::getLocker(opCtx.get());
    locker->setTicketQueueTime(waitForTickets);
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis, executionTime - waitForTickets);

    // Check that workintTimeMillis correctly account for time acquiring flow control tickets
    locker->setFlowControlTicketQueueTime(waitForFlowControlTicket);
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket);

    // Check that workingTimeMillis correctly accounts for lock wait time
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "CurOpStatsTest.Wait"));
    resetGlobalLockStats();

    Locker lockerConflict(getServiceContext());
    lockerConflict.lockGlobal(opCtx.get(), MODE_IX);
    ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });
    lockerConflict.lock(opCtx.get(), resId, MODE_X);
    {
        // This will be blocked
        locker->lockGlobal(opCtx.get(), MODE_IX);
        ON_BLOCK_EXIT([&] { locker->unlockGlobal(); });
        ASSERT_THROWS_CODE(
            locker->lock(opCtx.get(), resId, MODE_S, Date_t::now() + Milliseconds(8)),
            AssertionException,
            ErrorCodes::LockTimeout);
    }
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    Milliseconds waitForLocks =
        duration_cast<Milliseconds>(Microseconds(stats.get(resId, MODE_S).combinedWaitTimeMicros));

    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket - waitForLocks);
}
}  // namespace
}  // namespace mongo
