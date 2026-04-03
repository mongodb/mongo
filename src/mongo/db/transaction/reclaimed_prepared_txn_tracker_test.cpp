/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/transaction/reclaimed_prepared_txn_tracker.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

class ReclaimedPreparedTxnTrackerTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        _opCtx = getClient()->makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

using ReclaimedPreparedTxnTrackerDeathTest = ReclaimedPreparedTxnTrackerTest;

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             WaitInvariantsIfDiscoveryNotComplete,
             "Attempting to wait on all reclaimed prepared txns before discovery of reclaimed "
             "prepared txns is complete") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    SharedPromise<void> promise;
    tracker->beginDiscovery(1);
    tracker->trackPrepareExit(promise.getFuture());
    tracker->onAllReclaimedPreparedTxnsResolved().get(opCtx());
}

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             TrackPrepareExitInvariantsIfDiscoveryAlreadyComplete,
             "Attempting to track reclaimed prepared txn after discovery has completed") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    SharedPromise<void> promise;
    tracker->beginDiscovery(0);
    tracker->discoveryComplete();
    tracker->trackPrepareExit(promise.getFuture());
}

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             BeginDiscoveryInvariantsIfDiscoveryAlreadyBegun,
             "Beginning discovery of reclaimed prepared txns after it has already started") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->beginDiscovery(0);
    tracker->beginDiscovery(0);
}
DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             BeginDiscoveryInvariantsIfDiscoveryAlreadyComplete,
             "Beginning discovery of reclaimed prepared txns after it has already started") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->beginDiscovery(0);
    tracker->discoveryComplete();
    tracker->beginDiscovery(0);
}

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             DiscoveryCompleteInvariantsIfDiscoveryNotStarted,
             "Completing discovery of reclaimed prepared txns before explicitly starting it") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->discoveryComplete();
}

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             DiscoveryCompleteInvariantsIfDiscoveryAlreadyComplete,
             "Completing discovery of reclaimed prepared txns after it has already completed") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->beginDiscovery(0);
    tracker->discoveryComplete();
    tracker->discoveryComplete();
}

DEATH_TEST_F(ReclaimedPreparedTxnTrackerDeathTest,
             DiscoveryCompleteInvariantsIfExpectedPrepareExitsNotTracked,
             "Did not track the expected number of reclaimed prepared txns") {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->beginDiscovery(1);
    tracker->discoveryComplete();
}

TEST_F(ReclaimedPreparedTxnTrackerTest, WaitReturnsImmediatelyWhenNoFuturesTracked) {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    tracker->beginDiscovery(0);
    tracker->discoveryComplete();
    ASSERT_DOES_NOT_THROW(tracker->onAllReclaimedPreparedTxnsResolved().get(opCtx()));
}

TEST_F(ReclaimedPreparedTxnTrackerTest, WaitBlocksUntilAllTrackedFuturesResolveInOrder) {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    SharedPromise<void> promise1;
    SharedPromise<void> promise2;
    tracker->beginDiscovery(2);
    tracker->trackPrepareExit(promise1.getFuture());
    tracker->trackPrepareExit(promise2.getFuture());
    tracker->discoveryComplete();

    Notification<void> waiterAboutToWait;
    Notification<void> waiterDone;

    stdx::thread waiter([&] {
        auto client =
            getServiceContext()->getService()->makeClient("reclaimed-prepared-txn-waiter");
        AlternativeClientRegion acr(client);
        auto waiterOpCtx = cc().makeOperationContext();
        waiterAboutToWait.set();
        tracker->onAllReclaimedPreparedTxnsResolved().get(waiterOpCtx.get());
        waiterDone.set();
    });
    ScopeGuard joinGuard([&] { waiter.join(); });

    // Wait until the thread is about to call into the blocking wait.
    waiterAboutToWait.get();
    ASSERT_FALSE(waiterDone);

    // Resolve the first future; we should still block on the second.
    promise1.emplaceValue();
    ASSERT_FALSE(waiterDone);

    // Resolve the second future; now the wait should complete.
    promise2.emplaceValue();
    waiterDone.get();
}

TEST_F(ReclaimedPreparedTxnTrackerTest, PreparedTxnMayResolveBeforeDiscoveryCompletes) {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    SharedPromise<void> promise;

    tracker->beginDiscovery(1);

    // Make the future ready before tracking it to exercise the inline continuation path.
    promise.emplaceValue();
    tracker->trackPrepareExit(promise.getFuture());

    // Discovery completing after the prepared txn resolves should be safe.
    tracker->discoveryComplete();

    // The completion future should be ready immediately.
    auto allResolvedFuture = tracker->onAllReclaimedPreparedTxnsResolved();
    ASSERT_TRUE(allResolvedFuture.isReady());
    ASSERT_DOES_NOT_THROW(allResolvedFuture.get(opCtx()));
}

TEST_F(ReclaimedPreparedTxnTrackerTest,
       AllPreparedTxnsFutureNotReadyUntilTxnResolvedAfterDiscoveryCompletes) {
    auto* tracker = ReclaimedPreparedTxnTracker::get(getServiceContext());
    SharedPromise<void> resolvedDuringDiscovery;
    SharedPromise<void> resolvedAfterDiscovery;

    tracker->beginDiscovery(2);

    // Resolve one transaction during discovery (before discoveryComplete()).
    resolvedDuringDiscovery.emplaceValue();
    tracker->trackPrepareExit(resolvedDuringDiscovery.getFuture());

    // Track a second transaction that will resolve after discoveryComplete().
    tracker->trackPrepareExit(resolvedAfterDiscovery.getFuture());

    tracker->discoveryComplete();

    auto allResolvedFuture = tracker->onAllReclaimedPreparedTxnsResolved();
    ASSERT_FALSE(allResolvedFuture.isReady());

    // Only after the second transaction resolves should the completion future become ready.
    resolvedAfterDiscovery.emplaceValue();
    ASSERT_TRUE(allResolvedFuture.isReady());
    allResolvedFuture.get(opCtx());
}

TEST_F(ReclaimedPreparedTxnTrackerTest, RecoveryDurationMicrosRecordedCorrectly) {
    TickSourceMock<Microseconds> mockTickSource;
    ReclaimedPreparedTxnTracker tracker(&mockTickSource);

    tracker.beginDiscovery(0);
    mockTickSource.advance(Microseconds{5000});
    tracker.discoveryComplete();

    ASSERT_EQ(tracker.getRecoveryDurationMicros(), 5000);
}

}  // namespace
}  // namespace mongo
