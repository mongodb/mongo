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

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context_options_gen.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"
namespace mongo {

namespace {

class CurOpStatsTest : public ServiceContextTest {
protected:
    CurOpStatsTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()))) {}

    void advanceTime(Microseconds amount) {
        tickSource()->advance(amount);
    }

    TickSourceMock<Microseconds>* tickSource() {
        return checked_cast<decltype(tickSource())>(getServiceContext()->getTickSource());
    }

    void updateStatsOnTransactionUnstashWithLock(OperationContext* opCtx, CurOp* curop) {
        ClientLock lk(opCtx->getClient());
        curop->updateStatsOnTransactionUnstash(lk);
    }

    void updateStatsOnTransactionStashWithLock(OperationContext* opCtx, CurOp* curop) {
        ClientLock lk(opCtx->getClient());
        curop->updateStatsOnTransactionStash(lk);
    }
};

int64_t addWaitForLock(OperationContext* opCtx,
                       ServiceContext* svcCtx,
                       Locker* locker,
                       Milliseconds wait) {
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "CurOpStatsTest.WaitForLock"));
    resetGlobalLockStats();

    Locker lockerConflict(svcCtx);
    lockerConflict.lockGlobal(opCtx, MODE_IX);
    ON_BLOCK_EXIT([&] { lockerConflict.unlockGlobal(); });
    lockerConflict.lock(opCtx, resId, MODE_X);
    {
        // This will be blocked
        locker->lockGlobal(opCtx, MODE_IX);
        ON_BLOCK_EXIT([&] { locker->unlockGlobal(); });
        ASSERT_THROWS_CODE(locker->lock(opCtx, resId, MODE_S, Date_t::now() + wait),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    return stats.get(resId, MODE_S).combinedWaitTimeMicros;
}

void addTicketQueueTime(AdmissionContext* admCtx,
                        TickSourceMock<Microseconds>* tickSource,
                        Milliseconds& executionTime,
                        Milliseconds waitForTickets) {
    WaitingForAdmissionGuard waitForAdmission(admCtx, tickSource);
    tickSource->advance(waitForTickets);
    executionTime += waitForTickets;
}

TEST_F(CurOpStatsTest, CheckWorkingMillisValue) {
    auto opCtx = makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    auto* admCtx = &ExecutionAdmissionContext::get(opCtx.get());

    Milliseconds executionTime = Milliseconds(512);
    Milliseconds waitForTickets = Milliseconds(2);
    Milliseconds waitForFlowControlTicket = Milliseconds(4);

    // The prepare conflict tracker uses the service context's tick source to measure time,
    // so we'll make the service context and curop have the same tick source.
    // The tick source is set to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    advanceTime(Milliseconds{100});
    curop->setTickSource_forTest(tickSource());

    // Check that execution time advances as expected
    curop->ensureStarted();
    advanceTime(executionTime);
    curop->done();
    ASSERT_EQ(duration_cast<Milliseconds>(curop->elapsedTimeExcludingPauses()), executionTime);

    // Check that workingTimeMillis correctly accounts for ticket wait time
    auto locker = shard_role_details::getLocker(opCtx.get());
    addTicketQueueTime(admCtx, tickSource(), executionTime, waitForTickets);
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis, executionTime - waitForTickets);

    // Check that workingTimeMillis correctly account for time acquiring flow control tickets
    locker->addFlowControlTicketQueueTime(waitForFlowControlTicket);
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket);

    // Check that workingTimeMillis correctly accounts for lock wait time
    Milliseconds waitForLocks = duration_cast<Milliseconds>(
        Microseconds(addWaitForLock(opCtx.get(), getServiceContext(), locker, Milliseconds(8))));

    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket - waitForLocks);

    // Check that workingTimeMillis correctly excludes time spent waiting for prepare conflicts.
    // Simulate a prepare conflict and check that workingMillis is the same as before.
    StorageExecutionContext::get(opCtx.get())
        ->getPrepareConflictTracker()
        .beginPrepareConflict(*tickSource());
    advanceTime(Milliseconds(1000));
    StorageExecutionContext::get(opCtx.get())
        ->getPrepareConflictTracker()
        .endPrepareConflict(*tickSource());
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    // This wait time should be excluded from workingTimeMillis.
    ASSERT_EQ(curop->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket - waitForLocks);
}

TEST_F(CurOpStatsTest, UnstashingAndStashingTransactionResource) {
    // Initialize two operation contexts.
    auto serviceContext = getGlobalServiceContext();
    auto client1 = serviceContext->getService()->makeClient("client1");
    auto client2 = serviceContext->getService()->makeClient("client2");
    auto opCtx1 = client1->makeOperationContext();
    auto opCtx2 = client2->makeOperationContext();
    auto curop1 = CurOp::get(*opCtx1);
    auto curop2 = CurOp::get(*opCtx2);
    auto* admCtx1 = &ExecutionAdmissionContext::get(opCtx1.get());
    auto* admCtx2 = &ExecutionAdmissionContext::get(opCtx2.get());

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{100});
    curop1->setTickSource_forTest(tickSourceMock.get());
    curop2->setTickSource_forTest(tickSourceMock.get());

    // Advance execution time.
    Milliseconds executionTime = Milliseconds(1024);
    curop1->ensureStarted();
    curop2->ensureStarted();
    tickSourceMock->advance(executionTime);
    curop1->done();
    curop2->done();
    ASSERT_EQ(duration_cast<Milliseconds>(curop1->elapsedTimeExcludingPauses()), executionTime);
    ASSERT_EQ(duration_cast<Milliseconds>(curop2->elapsedTimeExcludingPauses()), executionTime);

    // Swap out opCtx1's locker with std::unique_ptr<Locker> so we can swap it around later on.
    shard_role_details::swapLocker(opCtx1.get(),
                                   std::make_unique<Locker>(opCtx1->getServiceContext()));

    // Check that ticket wait time on curop1 is not reported on curop2.
    Milliseconds waitForTickets = Milliseconds(2);
    addTicketQueueTime(admCtx1, tickSourceMock.get(), executionTime, waitForTickets);
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(admCtx1->totalTimeQueuedMicros(), waitForTickets);
    ASSERT_EQ(curop1->debug().workingTimeMillis, executionTime - waitForTickets);
    ASSERT_EQ(curop2->debug().workingTimeMillis, executionTime);

    // Check that flow control ticket wait time on curop1 is not reported on curop2.
    Milliseconds waitForFlowControlTicket = Milliseconds(32);
    auto lockerOp1 = shard_role_details::getLocker(opCtx1.get());
    lockerOp1->addFlowControlTicketQueueTime(waitForFlowControlTicket);
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(duration_cast<Milliseconds>(
                  Microseconds(lockerOp1->getFlowControlStats().timeAcquiringMicros)),
              waitForFlowControlTicket);
    ASSERT_EQ(curop1->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket);
    ASSERT_EQ(curop2->debug().workingTimeMillis, executionTime);

    // Check that lock wait time on curop1 is not reported on curop2.
    int64_t waitForLocks =
        addWaitForLock(opCtx1.get(), opCtx1->getServiceContext(), lockerOp1, Milliseconds(4));
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(lockerOp1->getLockerInfo(boost::none).stats.getCumulativeWaitTimeMicros(),
              waitForLocks);
    ASSERT_EQ(curop1->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket -
                  duration_cast<Milliseconds>(Microseconds(waitForLocks)));
    ASSERT_EQ(curop2->debug().workingTimeMillis, executionTime);

    // Simulate stashing locker from opCtx1. Check that wait times after stashing are still reported
    // on opCtx1.
    updateStatsOnTransactionStashWithLock(opCtx1.get(), curop1);
    auto locker = shard_role_details::swapLocker(
        opCtx1.get(), std::make_unique<Locker>(opCtx1->getServiceContext()));
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(duration_cast<Milliseconds>(
                  Microseconds(locker->getFlowControlStats().timeAcquiringMicros)),
              waitForFlowControlTicket);
    ASSERT_EQ(curop1->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket -
                  duration_cast<Milliseconds>(Microseconds(waitForLocks)));

    // Simulate unstashing transaction resources to opCtx2. Check that lock wait time on
    // unstashed locker is not considered blocked for opCtx2.
    shard_role_details::swapLocker(opCtx2.get(), std::move(locker));
    auto lockerOp2 = shard_role_details::getLocker(opCtx2.get());
    updateStatsOnTransactionUnstashWithLock(opCtx2.get(), curop2);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(lockerOp2->getLockerInfo(boost::none).stats.getCumulativeWaitTimeMicros(),
              waitForLocks);
    ASSERT_EQ(curop2->debug().workingTimeMillis, executionTime);

    // Check that workingTimeMillis correctly accounts for ticket wait duration on opCtx2.
    Milliseconds waitForTicketsOnOp2 = Milliseconds(8);
    addTicketQueueTime(admCtx2, tickSourceMock.get(), executionTime, waitForTicketsOnOp2);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(admCtx2->totalTimeQueuedMicros(), waitForTicketsOnOp2);
    ASSERT_EQ(curop2->debug().workingTimeMillis, executionTime - waitForTicketsOnOp2);

    // Check that workingTimeMillis correctly accounts for flow control ticket wait duration on
    // opCtx2.
    Milliseconds waitForFlowControlTicketsOnOp2 = Milliseconds(64);
    lockerOp2->addFlowControlTicketQueueTime(waitForFlowControlTicketsOnOp2);
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ((duration_cast<Milliseconds>(
                  Microseconds(lockerOp2->getFlowControlStats().timeAcquiringMicros))),
              waitForFlowControlTicket + waitForFlowControlTicketsOnOp2);
    ASSERT_EQ(curop2->debug().workingTimeMillis,
              executionTime - waitForTicketsOnOp2 - waitForFlowControlTicketsOnOp2);

    // Check that workingTimeMillis correctly accounts for lock wait duration on opCtx2.
    int64_t waitForLocksOnOp2 =
        addWaitForLock(opCtx2.get(), opCtx2->getServiceContext(), lockerOp2, Milliseconds(16));
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(lockerOp2->getLockerInfo(boost::none).stats.getCumulativeWaitTimeMicros(),
              waitForLocks + waitForLocksOnOp2);
    ASSERT_EQ(curop2->debug().workingTimeMillis,
              executionTime - waitForTicketsOnOp2 - waitForFlowControlTicketsOnOp2 -
                  duration_cast<Milliseconds>(Microseconds(waitForLocksOnOp2)));

    // Simulate stashing locker from opCtx2. Check that ticket and lock wait time after stashing is
    // still reported on opCtx2.
    updateStatsOnTransactionStashWithLock(opCtx2.get(), curop2);
    locker = shard_role_details::swapLocker(opCtx2.get(),
                                            std::make_unique<Locker>(opCtx2->getServiceContext()));
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop2->debug().workingTimeMillis,
              executionTime - waitForTicketsOnOp2 - waitForFlowControlTicketsOnOp2 -
                  duration_cast<Milliseconds>(Microseconds(waitForLocksOnOp2)));

    // Confirm stats are correctly accounted for even when we try unstashing the locker again.
    shard_role_details::swapLocker(opCtx2.get(), std::move(locker));
    updateStatsOnTransactionUnstashWithLock(opCtx2.get(), curop2);
    lockerOp2 = shard_role_details::getLocker(opCtx2.get());
    int64_t waitForLocksOnOp3 =
        addWaitForLock(opCtx2.get(), opCtx2->getServiceContext(), lockerOp2, Milliseconds(1));
    curop2->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop2->debug().workingTimeMillis,
              executionTime - waitForTicketsOnOp2 - waitForFlowControlTicketsOnOp2 -
                  duration_cast<Milliseconds>(Microseconds(waitForLocksOnOp2 + waitForLocksOnOp3)));
}

TEST_F(CurOpStatsTest, CheckWorkingMillisWithBlockedTimeAtStart) {
    auto opCtx = makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    auto* admCtx = &ExecutionAdmissionContext::get(opCtx.get());

    Milliseconds executionTime = Milliseconds(512);
    Milliseconds waitForTickets = Milliseconds(2);
    Milliseconds waitForFlowControlTicket = Milliseconds(4);

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{100});
    curop->setTickSource_forTest(tickSourceMock.get());

    // Add blocked time before curop timer starts.
    auto locker = shard_role_details::getLocker(opCtx.get());

    addTicketQueueTime(admCtx, tickSourceMock.get(), executionTime, waitForTickets);
    ASSERT_EQ(admCtx->totalTimeQueuedMicros(), waitForTickets);

    locker->addFlowControlTicketQueueTime(waitForFlowControlTicket);
    ASSERT_EQ(duration_cast<Milliseconds>(
                  Microseconds(locker->getFlowControlStats().timeAcquiringMicros)),
              waitForFlowControlTicket);

    int64_t waitForLocks =
        addWaitForLock(opCtx.get(), getServiceContext(), locker, Milliseconds(8));
    ASSERT_EQ(locker->getLockerInfo(boost::none).stats.getCumulativeWaitTimeMicros(), waitForLocks);

    // Advance execution time.
    curop->ensureStarted();
    tickSourceMock->advance(executionTime);
    curop->done();
    ASSERT_EQ(duration_cast<Milliseconds>(curop->elapsedTimeExcludingPauses()), executionTime);

    // Check that time blocked before curop timer starts is not subtracted from workingTimeMillis.
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop->debug().workingTimeMillis, executionTime);

    // Add blocked time after timer starts.
    addTicketQueueTime(admCtx, tickSourceMock.get(), executionTime, waitForTickets);
    locker->addFlowControlTicketQueueTime(waitForFlowControlTicket);
    waitForLocks = addWaitForLock(opCtx.get(), getServiceContext(), locker, Milliseconds(16));

    // Check that workingTimeMillis only accounts for blocked times after timer started.
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_APPROX_EQUAL(curop->debug().workingTimeMillis.count(),
                        (executionTime -
                         (waitForTickets + waitForFlowControlTicket +
                          duration_cast<Milliseconds>(Microseconds(waitForLocks))))
                            .count(),
                        1);
}

TEST_F(CurOpStatsTest, MultipleUnstashingAndStashingTransaction) {
    auto serviceContext = getGlobalServiceContext();
    auto client1 = serviceContext->getService()->makeClient("client1");
    auto opCtx1 = client1->makeOperationContext();
    auto curop1 = CurOp::get(*opCtx1);
    auto lockerOp1 = shard_role_details::getLocker(opCtx1.get());

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{123});
    curop1->setTickSource_forTest(tickSourceMock.get());

    // Advance execution time.
    curop1->ensureStarted();
    tickSourceMock->advance(Milliseconds{1000});
    lockerOp1->addFlowControlTicketQueueTime(Milliseconds{20});

    // Advance counters while stashed
    updateStatsOnTransactionStashWithLock(opCtx1.get(), curop1);
    tickSourceMock->advance(Milliseconds{1000});
    lockerOp1->addFlowControlTicketQueueTime(Milliseconds{30});
    updateStatsOnTransactionUnstashWithLock(opCtx1.get(), curop1);

    // Advance counters while not stashed
    tickSourceMock->advance(Milliseconds{1000});
    lockerOp1->addFlowControlTicketQueueTime(Milliseconds{40});

    // Advance counters while stashed
    updateStatsOnTransactionStashWithLock(opCtx1.get(), curop1);
    tickSourceMock->advance(Milliseconds{1000});
    lockerOp1->addFlowControlTicketQueueTime(Milliseconds{50});
    updateStatsOnTransactionUnstashWithLock(opCtx1.get(), curop1);

    // Advance counters while not stashed
    tickSourceMock->advance(Milliseconds{1000});
    lockerOp1->addFlowControlTicketQueueTime(Milliseconds{60});
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);

    // Verify that 120ms are excluded from 5000 execution time.
    // Out of 200ms of locker queue time, only 120ms happened while the locker was not stashed.
    ASSERT_EQ(curop1->debug().workingTimeMillis, Milliseconds(4880));
}

TEST_F(CurOpStatsTest, SubOperationStats) {
    auto opCtx = makeOperationContext();
    auto* admCtx = &ExecutionAdmissionContext::get(opCtx.get());
    auto locker = shard_role_details::getLocker(opCtx.get());
    auto curop1 = CurOp::get(*opCtx);

    Milliseconds executionTime = Milliseconds(512);
    Milliseconds waitForTickets = Milliseconds(2);
    Milliseconds waitForFlowControlTicket = Milliseconds(4);

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{100});
    curop1->setTickSource_forTest(tickSourceMock.get());

    // Advance execution time.
    curop1->ensureStarted();
    tickSourceMock->advance(executionTime);
    curop1->done();
    ASSERT_EQ(duration_cast<Milliseconds>(curop1->elapsedTimeExcludingPauses()), executionTime);

    // Add blocked time to curop1.
    addTicketQueueTime(admCtx, tickSourceMock.get(), executionTime, waitForTickets);
    locker->addFlowControlTicketQueueTime(waitForFlowControlTicket);
    Milliseconds waitForLocks = duration_cast<Milliseconds>(
        Microseconds(addWaitForLock(opCtx.get(), getServiceContext(), locker, Milliseconds(8))));
    curop1->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop1->debug().workingTimeMillis,
              executionTime - waitForTickets - waitForFlowControlTicket - waitForLocks);

    // Add new curop to top of curop stack using the same opCtx.
    CurOp curop2;
    curop2.push(opCtx.get());
    curop2.setTickSource_forTest(tickSourceMock.get());
    curop2.ensureStarted();

    // Check that curop2 only reports execution time that elapsed after it started without deducting
    // any blocked time from preceding operations in the stack.
    Milliseconds executionTime2 = Milliseconds(1024);
    tickSourceMock->advance(executionTime2);
    curop2.completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop2.debug().workingTimeMillis, executionTime2);

    // Add blocked time to curop2 and verify blocked time from curop1 is still not reported.
    Milliseconds waitForTickets2 = Milliseconds(16);
    Milliseconds waitForFlowControlTicket2 = Milliseconds(32);
    addTicketQueueTime(admCtx, tickSourceMock.get(), executionTime2, waitForTickets2);
    locker->addFlowControlTicketQueueTime(waitForFlowControlTicket2);
    auto waitForLocks2 = duration_cast<Milliseconds>(
        Microseconds(addWaitForLock(opCtx.get(), getServiceContext(), locker, Milliseconds(64))));
    curop2.completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(curop2.debug().workingTimeMillis,
              executionTime2 - waitForTickets2 - waitForFlowControlTicket2 - waitForLocks2);
}

TEST_F(CurOpStatsTest, CheckAdmissionQueueStats) {
    auto opCtx = makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    Milliseconds waitForIngressAdmission = Milliseconds(2);
    auto* ingressAdmCtx = &IngressAdmissionContext::get(opCtx.get());
    ingressAdmCtx->setAdmission_forTest(5);

    Milliseconds waitForExecutionTicket = Milliseconds(5);
    auto* executionAdmCtx = &ExecutionAdmissionContext::get(opCtx.get());
    executionAdmCtx->setAdmission_forTest(7);

    // initialize timer to non-zero value
    advanceTime(Milliseconds{100});
    curop->setTickSource_forTest(tickSource());

    Milliseconds executionTime = Milliseconds(512);
    advanceTime(executionTime);
    curop->done();

    // Add queueing time for ingress admission control
    addTicketQueueTime(ingressAdmCtx, tickSource(), executionTime, waitForIngressAdmission);

    BSONObjBuilder builder;
    SerializationContext sc = SerializationContext::stateCommandReply();
    sc.setPrefixState(false);
    {
        // Simulate operation currently waiting in execution control queue
        WaitingForAdmissionGuard waitForAdmission(executionAdmCtx, tickSource());
        tickSource()->advance(waitForExecutionTicket);
        executionTime += waitForExecutionTicket;

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curop->reportState(&builder, sc);
    }
    auto bsonObj = builder.done();
    BSONObj currentQueue = bsonObj.getObjectField("currentQueue");
    BSONObj queueStats = bsonObj.getObjectField("queues");

    auto expectedCurrentQueue = BSON("name" << "execution"
                                            << "timeQueuedMicros" << 5000);
    auto expectedQueueStats =
        BSON("execution" << BSON("admissions" << 7 << "totalTimeQueuedMicros" << 5000
                                              << "isHoldingTicket" << false)
                         << "ingress"
                         << BSON("admissions" << 5 << "totalTimeQueuedMicros" << 2000
                                              << "isHoldingTicket" << false));

    ASSERT_BSONOBJ_EQ(currentQueue, expectedCurrentQueue);
    ASSERT_BSONOBJ_EQ_UNORDERED(queueStats, expectedQueueStats);
}

TEST(CurOpTest, DelinquentInterruptChecksNotDoubleCounted) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);

    QueryTestServiceContext serviceContext;

    auto& totalInterruptChecks = CurOp::totalInterruptChecks_forTest();
    const auto interruptChecksAtStart = totalInterruptChecks.get();

    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    curop->ensureStarted();

    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};

    serviceContext.tickSource()->advance(interval * 2);
    opCtx->checkForInterrupt();

    // Now push another operation onto the curop stack.
    {
        CurOp subOp;
        subOp.push(opCtx.get());

        for (size_t i = 0; i < 10; ++i) {
            serviceContext.tickSource()->advance(interval * 10);
            opCtx->checkForInterrupt();
        }

        subOp.completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    }

    curop->done();
    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);

    ASSERT_EQ(totalInterruptChecks.get() - interruptChecksAtStart, 11);
}

TEST(CurOpTest, OpWhichNeverChecksForInterruptBumpsDelinquentCounter) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);

    QueryTestServiceContext serviceContext;

    auto& opsWithOverdueInterruptCheck = CurOp::opsWithOverdueInterruptCheck_forTest();
    const auto overdueOpsAtStart = opsWithOverdueInterruptCheck.get();

    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    curop->setTickSource_forTest(serviceContext.tickSource());
    curop->ensureStarted();

    // OpCtx should be set up to track interrupts.
    ASSERT(opCtx->overdueInterruptCheckStats());

    // The operation never checks for interrupt. It should still be marked as overdue though.
    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    serviceContext.tickSource()->advance(interval * 2);

    curop->completeAndLogOperation({logv2::LogComponent::kTest}, nullptr);
    ASSERT_EQ(opCtx->overdueInterruptCheckStats()->overdueInterruptChecks.loadRelaxed(), 1);

    ASSERT_EQ(opsWithOverdueInterruptCheck.get() - overdueOpsAtStart, 1);
}

TEST(CurOpTest, InterruptChecksSamplingRespectsFeatureFlag) {
    // When the feature flag is set to false, we should never sample an operation, even if the
    // sampling rate is set to 1.0
    RAIIServerParameterControllerForTest disableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", false);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);

    QueryTestServiceContext serviceContext;

    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    curop->ensureStarted();
    opCtx->checkForInterrupt();

    // Simulate advancing the mock clock.
    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    serviceContext.tickSource()->advance(interval * 5);

    // Verify that delinquent stats are not recorded due to the feature flag being disabled.
    ASSERT(!opCtx->overdueInterruptCheckStats());
    ASSERT_EQ(opCtx->numInterruptChecks(), 1);
}

TEST(CurOpTest, InterruptCheckTrackingWithSamplingRateZero) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    // When the sampling rate is 0, an operation should not track interrupts, but should
    // otherwise behave normally.
    RAIIServerParameterControllerForTest neverTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                              0);

    QueryTestServiceContext serviceContext;

    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    curop->ensureStarted();
    opCtx->checkForInterrupt();

    // Simulate advancing the mock clock.
    const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};
    serviceContext.tickSource()->advance(interval * 5);

    // Verify that overdue interrupt check stats are not tracked due to sampling rate being 0.
    ASSERT(!opCtx->overdueInterruptCheckStats());
    ASSERT_EQ(opCtx->numInterruptChecks(), 1);
}

TEST(CurOpTest, InterruptCheckTrackingIsSampled) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1.0 / 1000.0);

    QueryTestServiceContext serviceContext;

    size_t iters = 0;
    size_t nSampled = 0;

    do {
        auto opCtx = serviceContext.makeOperationContext();
        auto curop = CurOp::get(*opCtx);

        curop->ensureStarted();
        opCtx->checkForInterrupt();

        if (opCtx->overdueInterruptCheckStats()) {
            // Found an operation which was sampled.
            nSampled++;
            break;
        }

        ++iters;
    } while (iters < 100'000);

    // If this assertion fails, it means none of the CurOps we generated were selected
    // for sampling. This should be statistically impossible (around 1 in 10^44) assuming
    // 1 in 1000 ops gets randomly sampled, and indicates there's likely a problem
    // with the sampling mechanism (or you are very unlucky).
    ASSERT_GT(nSampled, 0);
}

}  // namespace
}  // namespace mongo
