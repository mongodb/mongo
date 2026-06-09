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

#include "mongo/db/stats/global_lock_stats.h"

#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>

namespace mongo {
namespace {

namespace ec = admission::execution_control;

class CollectGlobalLockSnapshotTest : public ServiceContextTest {
protected:
    void installTicketing(int numTickets) {
        const bool trackPeakUsed = false;
        constexpr auto maxQueueDepth = TicketHolder::kDefaultMaxQueueDepth;
        auto* svcCtx = getServiceContext();
        auto ticketingSystem = std::make_unique<ec::TicketingSystem>(
            svcCtx,
            ec::TicketingSystem::RWTicketHolder{
                std::make_unique<TicketHolder>(svcCtx, numTickets, trackPeakUsed, maxQueueDepth),
                std::make_unique<TicketHolder>(svcCtx, numTickets, trackPeakUsed, maxQueueDepth)},
            ec::TicketingSystem::RWTicketHolder{
                std::make_unique<TicketHolder>(svcCtx, numTickets, trackPeakUsed, maxQueueDepth),
                std::make_unique<TicketHolder>(svcCtx, numTickets, trackPeakUsed, maxQueueDepth)},
            ec::ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kFixedConcurrentTransactions);
        ec::TicketingSystem::use(svcCtx, std::move(ticketingSystem));
    }

    void clearTicketing() {
        ec::TicketingSystem::use(getServiceContext(), nullptr);
    }

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeClientsWithOperationContexts(size_t numClients) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(numClients);
        for (size_t i = 0; i < numClients; ++i) {
            auto client = getServiceContext()->getService()->makeClient(
                str::stream() << "global lock stats test client " << i);
            auto opCtx = client->makeOperationContext();
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
        return clients;
    }

    void waitForClientState(OperationContext* opCtx, Locker::ClientState expectedState) {
        auto* locker = shard_role_details::getLocker(opCtx);
        for (int i = 0; i < 10000; ++i) {
            if (locker->getClientState() == expectedState) {
                return;
            }
            sleepmillis(1);
        }
        ASSERT_EQ(expectedState, locker->getClientState());
    }
};

TEST_F(CollectGlobalLockSnapshotTest, ReportsZeroCountsWhenNoActiveClients) {
    const auto startedAt = Date_t::now();
    const auto snap = collectGlobalLockStatsSnapshot(startedAt);

    ASSERT_EQ(0, snap.activeReaders);
    ASSERT_EQ(0, snap.activeWriters);
    ASSERT_EQ(0, snap.queuedReaders);
    ASSERT_EQ(0, snap.queuedWriters);
    ASSERT_GTE(snap.totalTimeMicros, 0);
}

TEST_F(CollectGlobalLockSnapshotTest, TallyActiveAndQueuedReadersAndWriters) {
    installTicketing(/*numTickets=*/1);
    ScopeGuard restoreTicketing = [&] {
        clearTicketing();
    };

    auto clients = makeClientsWithOperationContexts(4);
    auto* activeReaderOpCtx = clients[0].second.get();
    auto* activeWriterOpCtx = clients[1].second.get();
    auto* queuedReaderOpCtx = clients[2].second.get();
    auto* queuedWriterOpCtx = clients[3].second.get();

    boost::optional<Lock::GlobalLock> activeReaderLock(
        boost::in_place_init, activeReaderOpCtx, MODE_IS);
    ASSERT(activeReaderLock->isLocked());

    boost::optional<Lock::GlobalLock> activeWriterLock(
        boost::in_place_init, activeWriterOpCtx, MODE_IX);
    ASSERT(activeWriterLock->isLocked());

    unittest::JoinThread queuedReaderThread{[&] {
        Lock::GlobalLock queuedReaderLock(queuedReaderOpCtx, MODE_IS);
    }};
    unittest::JoinThread queuedWriterThread{[&] {
        Lock::GlobalLock queuedWriterLock(queuedWriterOpCtx, MODE_IX);
    }};

    ScopeGuard cleanup = [&] {
        activeReaderLock.reset();
        activeWriterLock.reset();
    };

    waitForClientState(queuedReaderOpCtx, Locker::ClientState::kQueuedReader);
    waitForClientState(queuedWriterOpCtx, Locker::ClientState::kQueuedWriter);

    const auto snap = collectGlobalLockStatsSnapshot(Date_t::now());

    ASSERT_EQ(1, snap.activeReaders);
    ASSERT_EQ(1, snap.activeWriters);
    ASSERT_EQ(1, snap.queuedReaders);
    ASSERT_EQ(1, snap.queuedWriters);
}

TEST_F(CollectGlobalLockSnapshotTest, TotalTimeIncreasesFromStartedAt) {
    const auto startedAt = Date_t::now() - Milliseconds{50};
    const auto snap = collectGlobalLockStatsSnapshot(startedAt);
    ASSERT_GTE(snap.totalTimeMicros, durationCount<Microseconds>(Milliseconds{50}));
}

// Verifies that constructing and destroying Lockers (via operation contexts) correctly increments
// and decrements the global inactive counter.
TEST_F(CollectGlobalLockSnapshotTest, GlobalCountersTrackLockerLifecycle) {
    const auto before = Locker::getGlobalClientStateCounts();

    {
        auto clients = makeClientsWithOperationContexts(3);
        const auto during = Locker::getGlobalClientStateCounts();
        ASSERT_EQ(before.inactive + 3, during.inactive);
    }

    const auto after = Locker::getGlobalClientStateCounts();
    ASSERT_EQ(before.inactive, after.inactive);
}

// Verifies that the _setWaitingResource() code path correctly transitions global counters from
// active to queued when a lock acquisition blocks on the lock manager (not on ticket wait).
// This exercises the active->queued->active state machine that is separate from ticket queuing.
TEST_F(CollectGlobalLockSnapshotTest, GlobalCountersReflectLockContention) {
    // No ticketing installed so queuing happens only at the lock-manager level.
    auto clients = makeClientsWithOperationContexts(2);
    auto* holderOpCtx = clients[0].second.get();
    auto* waiterOpCtx = clients[1].second.get();

    const auto baseline = Locker::getGlobalClientStateCounts();

    boost::optional<Lock::GlobalLock> exclusiveLock(boost::in_place_init, holderOpCtx, MODE_X);
    ASSERT(exclusiveLock->isLocked());

    unittest::JoinThread waiterThread{[&] {
        Lock::GlobalLock readLock(waiterOpCtx, MODE_IS);
    }};

    // Release the exclusive lock (and thus unblock the waiter) before joining the thread.
    ScopeGuard releaseHolder = [&] {
        exclusiveLock.reset();
    };

    // _setWaitingResource() fires once the lock manager returns LOCK_WAITING and transitions
    // _clientState from kActiveReader to kQueuedReader, updating the global atomic counters.
    waitForClientState(waiterOpCtx, Locker::ClientState::kQueuedReader);

    const auto counts = Locker::getGlobalClientStateCounts();
    ASSERT_EQ(baseline.activeWriter + 1, counts.activeWriter);
    ASSERT_EQ(baseline.queuedReader + 1, counts.queuedReader);
}

// Verifies that getGlobalClientStateCounts() and collectGlobalLockStatsSnapshot() agree under
// ticket contention, confirming the snapshot function correctly delegates to the atomic counters.
TEST_F(CollectGlobalLockSnapshotTest, GlobalCountersMatchSnapshotUnderTicketContention) {
    installTicketing(/*numTickets=*/1);
    ScopeGuard restoreTicketing = [&] {
        clearTicketing();
    };

    auto clients = makeClientsWithOperationContexts(2);
    auto* readerOpCtx = clients[0].second.get();
    auto* queuedReaderOpCtx = clients[1].second.get();

    const auto baseline = Locker::getGlobalClientStateCounts();

    boost::optional<Lock::GlobalLock> readerLock(boost::in_place_init, readerOpCtx, MODE_IS);
    ASSERT(readerLock->isLocked());

    unittest::JoinThread waiterThread{[&] {
        Lock::GlobalLock waiterLock(queuedReaderOpCtx, MODE_IS);
    }};

    ScopeGuard releaseReader = [&] {
        readerLock.reset();
    };

    waitForClientState(queuedReaderOpCtx, Locker::ClientState::kQueuedReader);

    const auto counts = Locker::getGlobalClientStateCounts();
    ASSERT_EQ(baseline.activeReader + 1, counts.activeReader);
    ASSERT_EQ(baseline.queuedReader + 1, counts.queuedReader);

    const auto snap = collectGlobalLockStatsSnapshot(Date_t::now());
    ASSERT_EQ(counts.activeReader, snap.activeReaders);
    ASSERT_EQ(counts.queuedReader, snap.queuedReaders);
}

}  // namespace
}  // namespace mongo
