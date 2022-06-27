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


#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/ticketholders.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const int kMaxStressThreads = 32;  // max number of threads to use for lock stress
#ifdef _WIN32
const auto kMaxClockJitterMillis = Milliseconds(100);  // max backward jumps to tolerate
#else
const auto kMaxClockJitterMillis = Milliseconds(0);
#endif

/**
 * A RAII object that instantiates a TicketHolder that limits number of allowed global lock
 * acquisitions to numTickets. The opCtx must live as long as the UseGlobalThrottling instance.
 */
class UseGlobalThrottling {
public:
    explicit UseGlobalThrottling(OperationContext* opCtx, int numTickets) {
        auto* svcCtx = opCtx->getServiceContext();
        auto& ticketHolders = TicketHolders::get(svcCtx);
        ticketHolders.setGlobalThrottling(
            std::make_unique<SemaphoreTicketHolder>(numTickets, svcCtx),
            std::make_unique<SemaphoreTicketHolder>(numTickets, svcCtx));
        _ticketHolders = &ticketHolders;
    }
    ~UseGlobalThrottling() noexcept(false) {
        _ticketHolders->setGlobalThrottling(nullptr, nullptr);
    }

private:
    TicketHolders* _ticketHolders;
};


class DConcurrencyTestFixture : public ServiceContextMongoDTest {
public:
    /**
     * Returns a vector of Clients of length 'k', each of which has an OperationContext with its
     * lockState set to a LockerImpl.
     */
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeKClientsWithLockers(int k) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(k);
        for (int i = 0; i < k; ++i) {
            auto client =
                getServiceContext()->makeClient(str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            client->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
        return clients;
    }

    stdx::future<void> runTaskAndKill(OperationContext* opCtx,
                                      std::function<void()> fn,
                                      std::function<void()> postKill = nullptr) {
        auto task = stdx::packaged_task<void()>(fn);
        auto result = task.get_future();
        stdx::thread taskThread{std::move(task)};

        ScopeGuard taskThreadJoiner([&] { taskThread.join(); });

        {
            stdx::lock_guard<Client> clientLock(*opCtx->getClient());
            opCtx->markKilled();
        }

        if (postKill)
            postKill();

        return result;
    }

    void waitForLockerToHaveWaitingResource(Locker* locker) {
        while (!locker->getWaitingResource().isValid()) {
            sleepmillis(0);
        }
    }
};


TEST_F(DConcurrencyTestFixture, WriteConflictRetryInstantiatesOK) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    writeConflictRetry(opCtx.get(), "", "", [] {});
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryRetriesFunctionOnWriteConflictException) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto&& opDebug = CurOp::get(opCtx.get())->debug();
    ASSERT_EQUALS(0, opDebug.additiveMetrics.writeConflicts.load());
    ASSERT_EQUALS(100, writeConflictRetry(opCtx.get(), "", "", [&opDebug] {
                      if (0 == opDebug.additiveMetrics.writeConflicts.load()) {
                          throwWriteConflictException();
                      }
                      return 100;
                  }));
    ASSERT_EQUALS(1LL, opDebug.additiveMetrics.writeConflicts.load());
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryPropagatesNonWriteConflictException) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    ASSERT_THROWS_CODE(writeConflictRetry(opCtx.get(),
                                          "",
                                          "",
                                          [] {
                                              uassert(ErrorCodes::OperationFailed, "", false);
                                              MONGO_UNREACHABLE;
                                          }),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

TEST_F(DConcurrencyTestFixture,
       WriteConflictRetryPropagatesWriteConflictExceptionIfAlreadyInAWriteUnitOfWork) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::GlobalWrite globalWrite(opCtx.get());
    WriteUnitOfWork wuow(opCtx.get());
    ASSERT_THROWS(writeConflictRetry(opCtx.get(), "", "", [] { throwWriteConflictException(); }),
                  WriteConflictException);
}

TEST_F(DConcurrencyTestFixture, ResourceMutex) {
    Lock::ResourceMutex mtx("testMutex");
    auto opCtx = makeOperationContext();
    LockerImpl locker1(opCtx->getServiceContext());
    LockerImpl locker2(opCtx->getServiceContext());
    LockerImpl locker3(opCtx->getServiceContext());

    struct State {
        void check(int n) {
            ASSERT_EQ(step.load(), n);
        }
        void finish(int n) {
            auto actual = step.fetchAndAdd(1);
            ASSERT_EQ(actual, n);
        }
        void waitFor(std::function<bool()> cond) {
            while (!cond())
                sleepmillis(0);
        }
        void waitFor(int n) {
            waitFor([this, n]() { return this->step.load() == n; });
        }
        AtomicWord<int> step{0};
    } state;

    stdx::thread t1([&]() {
        // Step 0: Single thread acquires shared lock
        state.waitFor(0);
        Lock::SharedLock lk(&locker1, mtx);
        ASSERT(lk.isLocked());
        state.finish(0);

        // Step 4: Wait for t2 to regain its shared lock
        {
            state.waitFor(4);
            state.waitFor([&locker2]() { return locker2.getWaitingResource().isValid(); });
            state.finish(4);
        }

        // Step 5: After t2 becomes blocked, unlock, yielding the mutex to t3
        lk.unlock();
        ASSERT(!lk.isLocked());
    });
    stdx::thread t2([&]() {
        // Step 1: Two threads acquire shared lock
        state.waitFor(1);
        Lock::SharedLock lk(&locker2, mtx);
        ASSERT(lk.isLocked());
        state.finish(1);

        // Step 2: Wait for t3 to attempt the exclusive lock
        state.waitFor([&locker3]() { return locker3.getWaitingResource().isValid(); });
        state.finish(2);

        // Step 3: Yield shared lock
        lk.unlock();
        ASSERT(!lk.isLocked());
        state.finish(3);

        // Step 4: Try to regain the shared lock // transfers control to t1
        lk.lock(nullptr, MODE_IS);

        // Step 6: CHeck we actually got back the shared lock
        ASSERT(lk.isLocked());
        state.check(6);
    });
    stdx::thread t3([&]() {
        // Step 2: Third thread attempts to acquire exclusive lock
        state.waitFor(2);
        Lock::ExclusiveLock lk(&locker3, mtx);  // transfers control to t2

        // Step 5: Actually get the exclusive lock
        ASSERT(lk.isLocked());
        state.finish(5);
    });
    t1.join();
    t2.join();
    t3.join();
}

TEST_F(DConcurrencyTestFixture, GlobalRead) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::GlobalRead globalRead(opCtx.get());
    ASSERT(opCtx->lockState()->isR());
    ASSERT_EQ(opCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
}

TEST_F(DConcurrencyTestFixture, GlobalWrite) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(opCtx->lockState()->isW());
    ASSERT_EQ(opCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
}

TEST_F(DConcurrencyTestFixture, GlobalWriteAndGlobalRead) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(lockState->isW());

    {
        Lock::GlobalRead globalRead(opCtx.get());
        ASSERT(lockState->isW());
    }

    ASSERT(lockState->isW());
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
}

TEST_F(DConcurrencyTestFixture,
       GlobalWriteRequiresExplicitDowngradeToIntentWriteModeIfDestroyedWhileHoldingDatabaseLock) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    auto globalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "db"), MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));

        // If we destroy the GlobalWrite out of order relative to the DBLock, we will leave the
        // global lock resource locked in MODE_X. We have to explicitly downgrade this resource to
        // MODE_IX to allow other write operations to make progress.
        // This test case illustrates non-recommended usage of the RAII types. See SERVER-30948.
        globalWrite = {};
        ASSERT(lockState->isW());
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

        lockState->downgrade(resourceIdGlobal, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    }


    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
}

TEST_F(DConcurrencyTestFixture,
       GlobalWriteRequiresSupportsDowngradeToIntentWriteModeWhileHoldingDatabaseLock) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    auto globalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "db"), MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

        // Downgrade global lock resource to MODE_IX to allow other write operations to make
        // progress.
        lockState->downgrade(resourceIdGlobal, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    }

    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    globalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
}

TEST_F(DConcurrencyTestFixture,
       NestedGlobalWriteSupportsDowngradeToIntentWriteModeWhileHoldingDatabaseLock) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    auto outerGlobalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    auto innerGlobalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "db"), MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

        // Downgrade global lock resource to MODE_IX to allow other write operations to make
        // progress.
        lockState->downgrade(resourceIdGlobal, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(resourceIdGlobal))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
        ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    }

    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    innerGlobalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());
    ASSERT(MODE_IX == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    outerGlobalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_Timeout) {
    auto clients = makeKClientsWithLockers(2);

    Lock::GlobalLock globalWrite(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite.isLocked());

    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_S,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_Timeout) {
    auto clients = makeKClientsWithLockers(2);
    Lock::GlobalLock globalWrite(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite.isLocked());

    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, RSTLmodeX_Timeout) {
    auto clients = makeKClientsWithLockers(2);
    Lock::ResourceLock rstl(
        clients[0].second.get()->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);
    ASSERT_EQ(
        clients[0].second.get()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
        MODE_X);

    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
    ASSERT_EQ(
        clients[0].second.get()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
        MODE_X);
    ASSERT_EQ(
        clients[1].second.get()->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
        MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, PBWMmodeX_Timeout) {
    auto clients = makeKClientsWithLockers(2);
    Lock::ParallelBatchWriterMode pbwm(clients[0].second.get()->lockState());
    ASSERT_EQ(clients[0].second.get()->lockState()->getLockMode(resourceIdParallelBatchWriterMode),
              MODE_X);

    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
    ASSERT_EQ(clients[0].second.get()->lockState()->getLockMode(resourceIdParallelBatchWriterMode),
              MODE_X);
    ASSERT_EQ(clients[1].second.get()->lockState()->getLockMode(resourceIdParallelBatchWriterMode),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());

    {
        Lock::GlobalLock globalWrite(opCtx, MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockIXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalWrite(
            opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockSDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_S, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockISDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());

    { Lock::DBLock dbWrite(opCtx, DatabaseName(boost::none, "db"), MODE_X); }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockSDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());

    { Lock::DBLock dbRead(opCtx, DatabaseName(boost::none, "db"), MODE_S); }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockXDoesNotSetGlobalWriteLockedWhenLockAcquisitionTimesOut) {
    auto clients = makeKClientsWithLockers(2);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = clients[1].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());
    {
        ASSERT_THROWS_CODE(
            Lock::GlobalLock(
                opCtx, MODE_X, Date_t::now() + Milliseconds(1), Lock::InterruptBehavior::kThrow),
            AssertionException,
            ErrorCodes::LockTimeout);
    }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockSSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::GlobalLock globalWrite(opCtx, MODE_S, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockISDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockIXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockSDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    { Lock::DBLock dbWrite(opCtx, DatabaseName(boost::none, "db"), MODE_S); }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockISDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    { Lock::DBLock dbWrite(opCtx, DatabaseName(boost::none, "db"), MODE_IS); }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockIXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    { Lock::DBLock dbWrite(opCtx, DatabaseName(boost::none, "db"), MODE_IX); }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    { Lock::DBLock dbRead(opCtx, DatabaseName(boost::none, "db"), MODE_X); }
    ASSERT_TRUE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture,
       GlobalLockSDoesNotSetGlobalLockTakenInModeConflictingWithWritesWhenLockAcquisitionTimesOut) {
    auto clients = makeKClientsWithLockers(2);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = clients[1].second.get();
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        ASSERT_THROWS_CODE(
            Lock::GlobalLock(
                opCtx, MODE_X, Date_t::now() + Milliseconds(1), Lock::InterruptBehavior::kThrow),
            AssertionException,
            ErrorCodes::LockTimeout);
    }
    ASSERT_FALSE(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_NoTimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    Lock::GlobalLock globalReadTry(clients[1].second.get(),
                                   MODE_S,
                                   Date_t::now() + Milliseconds(1),
                                   Lock::InterruptBehavior::kThrow);

    ASSERT(globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_S,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitIsInterruptible) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::GlobalLock GlobalLock(opCtx1, MODE_X);

    auto result = runTaskAndKill(opCtx2, [&]() {
        // Killing the lock wait should throw an exception.
        Lock::GlobalLock g(opCtx2, MODE_S);
    });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitIsInterruptibleBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::ResourceLock rstl(opCtx1->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);

    auto result = runTaskAndKill(opCtx2, [&]() {
        // Killing the lock wait should throw an exception.
        Lock::GlobalLock g(opCtx2, MODE_S);
    });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_X);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitIsInterruptibleMMAP) {
    auto clients = makeKClientsWithLockers(2);

    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::GlobalLock GlobalLock(opCtx1, MODE_X);

    // This thread attemps to acquire a conflicting lock, which will block until the first
    // unlocks.
    auto result = runTaskAndKill(opCtx2, [&]() {
        // Killing the lock wait should throw an exception.
        Lock::GlobalLock g(opCtx2, MODE_S);
    });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitNotInterruptedWithLeaveUnlockedBehavior) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::GlobalLock g1(opCtx1, MODE_X);
    // Acquire this later to confirm that it stays unlocked.
    boost::optional<Lock::GlobalLock> g2 = boost::none;

    // Killing the lock wait should not interrupt it, but rather leave it lock unlocked.
    auto result = runTaskAndKill(opCtx2, [&]() {
        g2.emplace(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);
    });
    ASSERT(g1.isLocked());
    ASSERT(g2 != boost::none);
    ASSERT(!g2->isLocked());
    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Should not throw an exception.
    result.get();
}

TEST_F(DConcurrencyTestFixture,
       GlobalLockWaitNotInterruptedWithLeaveUnlockedBehaviorBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::ResourceLock rstl(opCtx1->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);
    // Acquire this later to confirm that it stays unlocked.
    boost::optional<Lock::GlobalLock> g2 = boost::none;

    // Killing the lock wait should not interrupt it, but rather leave it lock unlocked.
    auto result = runTaskAndKill(opCtx2, [&]() {
        g2.emplace(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);
    });
    ASSERT(g2 != boost::none);
    ASSERT(!g2->isLocked());
    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_X);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Should not throw an exception.
    result.get();
}

TEST_F(DConcurrencyTestFixture, GlobalLockNotInterruptedWithLeaveUnlockedBehavior) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();

    // Kill the operation before acquiring the uncontested lock.
    {
        stdx::lock_guard<Client> clientLock(*opCtx1->getClient());
        opCtx1->markKilled();
    }
    // This should not throw or acquire the lock.
    Lock::GlobalLock g1(opCtx1, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);
    ASSERT(!g1.isLocked());
}

TEST_F(DConcurrencyTestFixture, SetMaxLockTimeoutMillisAndDoNotUsingWithInterruptBehavior) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::GlobalLock g1(opCtx1, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown
    // and then caught by the Lock::InterruptBehavior::kLeaveUnlocked setting.
    opCtx2->lockState()->setMaxLockTimeout(Milliseconds(100));
    Lock::GlobalLock g2(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);

    ASSERT(g1.isLocked());
    ASSERT(!g2.isLocked());

    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, SetMaxLockTimeoutMillisAndNotUsingInterruptBehaviorBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::ResourceLock rstl(opCtx1->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown
    // and then caught by the Lock::InterruptBehavior::kLeaveUnlocked setting.
    opCtx2->lockState()->setMaxLockTimeout(Milliseconds(100));
    Lock::GlobalLock g2(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);

    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_X);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    ASSERT(!g2.isLocked());
}

TEST_F(DConcurrencyTestFixture, SetMaxLockTimeoutMillisAndThrowUsingInterruptBehavior) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::GlobalLock g1(opCtx1, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown.
    opCtx2->lockState()->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT(g1.isLocked());

    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture,
       SetMaxLockTimeoutMillisAndThrowUsingInterruptBehaviorBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::ResourceLock rstl(opCtx1->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown.
    opCtx2->lockState()->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceIdReplicationStateTransitionLock), MODE_X);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, FailedGlobalLockShouldUnlockRSTLOnlyOnce) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto resourceRSTL = resourceIdReplicationStateTransitionLock;

    // Take the exclusive lock with the first caller.
    Lock::GlobalLock globalLock(opCtx1, MODE_X);

    opCtx2->lockState()->beginWriteUnitOfWork();
    // Set a max timeout on the second caller that will override provided lock request
    // deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown.
    opCtx2->lockState()->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_IX, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);
    auto opCtx2Locker = static_cast<LockerImpl*>(opCtx2->lockState());
    // GlobalLock failed, but the RSTL should be successfully acquired and pending unlocked.
    ASSERT(opCtx2Locker->getRequestsForTest().find(resourceIdGlobal).finished());
    ASSERT_EQ(opCtx2Locker->getRequestsForTest().find(resourceRSTL).objAddr()->unlockPending, 1U);
    ASSERT_EQ(opCtx2Locker->getRequestsForTest().find(resourceRSTL).objAddr()->recursiveCount, 1U);
    opCtx2->lockState()->endWriteUnitOfWork();
    ASSERT_EQ(opCtx1->lockState()->getLockMode(resourceRSTL), MODE_IX);
    ASSERT_EQ(opCtx2->lockState()->getLockMode(resourceRSTL), MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, DBLockWaitIsInterruptible) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    DatabaseName dbName(boost::none, "db");
    Lock::DBLock dbLock(opCtx1, dbName, MODE_X);

    auto result = runTaskAndKill(opCtx2, [&]() {
        // This lock conflicts with the other DBLock.
        Lock::DBLock d(opCtx2, dbName, MODE_S);
    });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitIsNotInterruptibleWithLockGuard) {
    auto clients = makeKClientsWithLockers(2);

    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();
    // The main thread takes an exclusive lock, causing the spawned thread wait when it attempts to
    // acquire a conflicting lock.
    boost::optional<Lock::GlobalLock> globalLock = Lock::GlobalLock(opCtx1, MODE_X);

    // Killing the lock wait should not interrupt it.
    auto result = runTaskAndKill(opCtx2,
                                 [&]() {
                                     UninterruptibleLockGuard noInterrupt(opCtx2->lockState());
                                     Lock::GlobalLock g(opCtx2, MODE_S);
                                 },
                                 [&]() { globalLock.reset(); });
    // Should not throw an exception.
    result.get();
}

TEST_F(DConcurrencyTestFixture, DBLockWaitIsNotInterruptibleWithLockGuard) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    boost::optional<Lock::DBLock> dbLock =
        Lock::DBLock(opCtx1, DatabaseName(boost::none, "db"), MODE_X);

    // Killing the lock wait should not interrupt it.
    auto result =
        runTaskAndKill(opCtx2,
                       [&]() {
                           UninterruptibleLockGuard noInterrupt(opCtx2->lockState());
                           Lock::DBLock d(opCtx2, DatabaseName(boost::none, "db"), MODE_S);
                       },
                       [&] { dbLock.reset(); });
    // Should not throw an exception.
    result.get();
}

TEST_F(DConcurrencyTestFixture, LockCompleteInterruptedWhenUncontested) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opCtx1 = clientOpctxPairs[0].second.get();
    auto opCtx2 = clientOpctxPairs[1].second.get();

    boost::optional<repl::ReplicationStateTransitionLockGuard> lockXGranted;
    lockXGranted.emplace(opCtx1, MODE_IX, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(lockXGranted->isLocked());

    // Attempt to take a conflicting lock, which will fail.
    LockResult result = opCtx2->lockState()->lockRSTLBegin(opCtx2, MODE_X);
    ASSERT_EQ(result, LOCK_WAITING);

    // Release the conflicting lock.
    lockXGranted.reset();

    {
        stdx::lock_guard<Client> clientLock(*opCtx2->getClient());
        opCtx2->markKilled();
    }

    // After the operation has been killed, the lockComplete request should fail, even though the
    // lock is uncontested.
    ASSERT_THROWS_CODE(opCtx2->lockState()->lockRSTLComplete(opCtx2, MODE_X, Date_t::max()),
                       AssertionException,
                       ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesS) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbRead(opCtx.get(), DatabaseName(boost::none, "db"), MODE_S);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesX) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "db"), MODE_X);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesISForAdminIS) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbRead(opCtx.get(), DatabaseName(boost::none, "admin"), MODE_IS);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_IS);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesSForAdminS) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbRead(opCtx.get(), DatabaseName(boost::none, "admin"), MODE_S);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesIXForAdminIX) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "admin"), MODE_IX);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_IX);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminX) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    Lock::DBLock dbWrite(opCtx.get(), DatabaseName(boost::none, "admin"), MODE_X);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, MultipleWriteDBLocksOnSameThread) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    DatabaseName dbName(boost::none, "db1");
    Lock::DBLock r1(opCtx.get(), dbName, MODE_X);
    Lock::DBLock r2(opCtx.get(), dbName, MODE_X);

    ASSERT(opCtx->lockState()->isDbLockedForMode("db1", MODE_X));
}

TEST_F(DConcurrencyTestFixture, MultipleConflictingDBLocksOnSameThread) {
    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();
    DatabaseName dbName(boost::none, "db1");
    Lock::DBLock r1(opCtx.get(), dbName, MODE_X);
    Lock::DBLock r2(opCtx.get(), dbName, MODE_S);

    ASSERT(lockState->isDbLockedForMode("db1", MODE_X));
    ASSERT(lockState->isDbLockedForMode("db1", MODE_S));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForSMode) {
    const std::string dbName("db");

    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), DatabaseName(boost::none, "db"), MODE_S);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForXMode) {
    const std::string dbName("db");

    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), DatabaseName(boost::none, "db"), MODE_X);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IS) {
    const NamespaceString ns("db1.coll");

    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), ns.dbName(), MODE_IS);

    {
        Lock::CollectionLock collLock(opCtx.get(), ns, MODE_IS);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(opCtx.get(), ns, MODE_S);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IX) {
    const NamespaceString ns("db1.coll");

    auto opCtx = makeOperationContext();
    getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), ns.dbName(), MODE_IX);

    {
        Lock::CollectionLock collLock(opCtx.get(), ns, MODE_IX);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(opCtx.get(), ns, MODE_X);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, Stress) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers(kMaxStressThreads);

    AtomicWord<int> ready{0};
    std::vector<stdx::thread> threads;


    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                if (i % 7 == 0 && threadId == 0 /* Only one upgrader legal */) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 1) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 2) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 3) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    Lock::GlobalRead r(clients[threadId].second.get());

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 4) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    Lock::GlobalRead r2(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 5) {
                    {
                        Lock::DBLock r(clients[threadId].second.get(),
                                       DatabaseName(boost::none, "foo"),
                                       MODE_S);
                    }
                    {
                        Lock::DBLock r(clients[threadId].second.get(),
                                       DatabaseName(boost::none, "bar"),
                                       MODE_S);
                    }
                } else if (i % 7 == 6) {
                    if (i > kNumIterations / 2) {
                        int q = i % 11;

                        if (q == 0) {
                            Lock::DBLock r(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo"),
                                           MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r2(clients[threadId].second.get(),
                                            DatabaseName(boost::none, "foo"),
                                            MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r3(clients[threadId].second.get(),
                                            DatabaseName(boost::none, "local"),
                                            MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "local", MODE_S));
                        } else if (q == 1) {
                            // test locking local only -- with no preceding lock
                            {
                                Lock::DBLock x(clients[threadId].second.get(),
                                               DatabaseName(boost::none, "local"),
                                               MODE_S);
                            }

                            Lock::DBLock x(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "local"),
                                           MODE_X);

                        } else if (q == 2) {
                            {
                                Lock::DBLock x(clients[threadId].second.get(),
                                               DatabaseName(boost::none, "admin"),
                                               MODE_S);
                            }
                            {
                                Lock::DBLock x(clients[threadId].second.get(),
                                               DatabaseName(boost::none, "admin"),
                                               MODE_X);
                            }
                        } else if (q == 3) {
                            Lock::DBLock x(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo"),
                                           MODE_X);
                            Lock::DBLock y(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "admin"),
                                           MODE_S);
                        } else if (q == 4) {
                            Lock::DBLock x(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo2"),
                                           MODE_S);
                            Lock::DBLock y(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "admin"),
                                           MODE_S);
                        } else if (q == 5) {
                            Lock::DBLock x(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo"),
                                           MODE_IS);
                        } else if (q == 6) {
                            Lock::DBLock x(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo"),
                                           MODE_IX);
                            Lock::DBLock y(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "local"),
                                           MODE_IX);
                        } else {
                            Lock::DBLock w(clients[threadId].second.get(),
                                           DatabaseName(boost::none, "foo"),
                                           MODE_X);

                            Lock::DBLock r2(clients[threadId].second.get(),
                                            DatabaseName(boost::none, "foo"),
                                            MODE_S);
                            Lock::DBLock r3(clients[threadId].second.get(),
                                            DatabaseName(boost::none, "local"),
                                            MODE_S);
                        }
                    } else {
                        Lock::DBLock r(clients[threadId].second.get(),
                                       DatabaseName(boost::none, "foo"),
                                       MODE_S);
                        Lock::DBLock r2(clients[threadId].second.get(),
                                        DatabaseName(boost::none, "foo"),
                                        MODE_S);
                        Lock::DBLock r3(clients[threadId].second.get(),
                                        DatabaseName(boost::none, "local"),
                                        MODE_S);
                    }
                }

                if (threadId == kMaxStressThreads - 1)
                    progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers(2);
    { Lock::GlobalWrite w(newClients[0].second.get()); }
    { Lock::GlobalRead r(newClients[1].second.get()); }
}

TEST_F(DConcurrencyTestFixture, StressPartitioned) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers(kMaxStressThreads);

    AtomicWord<int> ready{0};
    std::vector<stdx::thread> threads;

    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                if (threadId == 0) {
                    if (i % 100 == 0) {
                        Lock::GlobalWrite w(clients[threadId].second.get());
                        continue;
                    } else if (i % 100 == 1) {
                        Lock::GlobalRead w(clients[threadId].second.get());
                        continue;
                    }

                    // Intentional fall through
                }

                if (i % 2 == 0) {
                    Lock::DBLock x(
                        clients[threadId].second.get(), DatabaseName(boost::none, "foo"), MODE_IS);
                } else {
                    Lock::DBLock x(
                        clients[threadId].second.get(), DatabaseName(boost::none, "foo"), MODE_IX);
                    Lock::DBLock y(clients[threadId].second.get(),
                                   DatabaseName(boost::none, "local"),
                                   MODE_IX);
                }

                if (threadId == kMaxStressThreads - 1)
                    progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers(2);
    { Lock::GlobalWrite w(newClients[0].second.get()); }
    { Lock::GlobalRead r(newClients[1].second.get()); }
}

TEST_F(DConcurrencyTestFixture, ResourceMutexLabels) {
    Lock::ResourceMutex mutex("label");
    ASSERT(mutex.getName() == "label");
    Lock::ResourceMutex mutex2("label2");
    ASSERT(mutex2.getName() == "label2");
}

TEST_F(DConcurrencyTestFixture, Throttling) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    UseGlobalThrottling throttle(opctx1, 1);

    bool overlongWait;
    int tries = 0;
    const int maxTries = 15;
    const Milliseconds timeoutMillis = Milliseconds(42);

    do {
        // Test that throttling will correctly handle timeouts.
        Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R1.isLocked());

        Date_t t1 = Date_t::now();
        {
            ASSERT_THROWS_CODE(Lock::GlobalRead(opctx2,
                                                Date_t::now() + timeoutMillis,
                                                Lock::InterruptBehavior::kThrow),
                               AssertionException,
                               ErrorCodes::LockTimeout);
        }
        Date_t t2 = Date_t::now();

        // Test that the timeout did result in at least the requested wait.
        ASSERT_GTE(t2 - t1 + kMaxClockJitterMillis, timeoutMillis);

        // Timeouts should be reasonably immediate. In maxTries attempts at least one test should be
        // able to complete within a second, as the theoretical test duration is less than 50 ms.
        overlongWait = t2 - t1 >= Seconds(1);
    } while (overlongWait && ++tries < maxTries);
    ASSERT(!overlongWait);
}

TEST_F(DConcurrencyTestFixture, NoThrottlingWhenNotAcquiringTickets) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opctx1, 1);

    // Prevent the enforcement of ticket throttling.
    opctx1->lockState()->skipAcquireTicket();

    // Both locks should be acquired immediately because there is no throttling.
    Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1.isLocked());

    Lock::GlobalRead R2(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R2.isLocked());
}

TEST_F(DConcurrencyTestFixture, ReleaseAndReacquireTicket) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opctx1, 1);

    Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1.isLocked());

    {
        // A second Locker should not be able to acquire a ticket.
        ASSERT_THROWS_CODE(Lock::GlobalRead(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }

    opctx1->lockState()->releaseTicket();

    {
        // Now a second Locker can acquire a ticket.
        Lock::GlobalRead R2(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R2.isLocked());
    }

    opctx1->lockState()->reacquireTicket(opctx1);

    {
        // Now a second Locker cannot acquire a ticket.
        ASSERT_THROWS_CODE(Lock::GlobalRead(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }
}

TEST_F(DConcurrencyTestFixture, LockerWithReleasedTicketCanBeUnlocked) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();

    Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1.isLocked());

    opctx1->lockState()->releaseTicket();
}

TEST_F(DConcurrencyTestFixture, TicketAcquireCanThrowDueToKill) {
    auto clientOpctxPairs = makeKClientsWithLockers(1);
    auto opctx1 = clientOpctxPairs[0].second.get();
    // Limit the locker to 0 tickets at a time.
    UseGlobalThrottling throttle(opctx1, 0);

    // This thread should block because it cannot acquire a ticket and then get interrupted.
    auto result = runTaskAndKill(opctx1, [&] { Lock::GlobalRead R2(opctx1); });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireCanThrowDueToMaxLockTimeout) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    UseGlobalThrottling throttle(opCtx, 0);

    opCtx->lockState()->setMaxLockTimeout(Milliseconds(100));
    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx, MODE_IX), AssertionException, ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireCanThrowDueToDeadline) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    UseGlobalThrottling throttle(opCtx, 0);
    ASSERT_THROWS_CODE(
        Lock::GlobalLock(
            opCtx, MODE_IX, Date_t::now() + Milliseconds(1500), Lock::InterruptBehavior::kThrow),
        AssertionException,
        ErrorCodes::LockTimeout);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireShouldNotThrowIfBehaviorIsLeaveUnlocked1) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    UseGlobalThrottling throttle(opCtx, 0);

    opCtx->lockState()->setMaxLockTimeout(Milliseconds(100));
    Lock::GlobalLock(opCtx, MODE_IX, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireShouldNotThrowIfBehaviorIsLeaveUnlocked2) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    UseGlobalThrottling throttle(opCtx, 0);
    boost::optional<Lock::GlobalLock> globalLock;
    globalLock.emplace(opCtx,
                       MODE_IX,
                       Date_t::now() + Milliseconds(1500),
                       Lock::InterruptBehavior::kLeaveUnlocked);
    ASSERT(!globalLock->isLocked());
}

TEST_F(DConcurrencyTestFixture, TicketAcquireWithMaxDeadlineRespectsUninterruptibleLockGuard) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opCtx1 = clientOpctxPairs[0].second.get();
    auto opCtx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opCtx1, 1);

    // Take the only ticket available.
    boost::optional<Lock::GlobalRead> R1;
    R1.emplace(opCtx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1->isLocked());

    boost::optional<Lock::GlobalRead> R2;

    // Block until a ticket is available.
    auto result =
        runTaskAndKill(opCtx2,
                       [&] {
                           UninterruptibleLockGuard noInterrupt(opCtx2->lockState());
                           R2.emplace(opCtx2, Date_t::max(), Lock::InterruptBehavior::kThrow);
                       },
                       [&] {
                           // Relase the only ticket available to unblock the other thread.
                           R1.reset();
                       });

    result.get();  // This should not throw.
    ASSERT(R2->isLocked());
}

TEST_F(DConcurrencyTestFixture, TicketReacquireCanBeInterrupted) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opctx1, 1);

    Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1.isLocked());

    {
        // A second Locker should not be able to acquire a ticket.

        ASSERT_THROWS_CODE(Lock::GlobalRead(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }

    opctx1->lockState()->releaseTicket();

    // Now a second Locker can acquire a ticket.
    Lock::GlobalRead R2(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R2.isLocked());

    // This thread should block because it cannot acquire a ticket.
    auto result = runTaskAndKill(opctx1, [&] { opctx1->lockState()->reacquireTicket(opctx1); });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, GlobalLockInInterruptedContextThrowsEvenWhenUncontested) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    boost::optional<Lock::GlobalRead> globalReadLock;
    ASSERT_THROWS_CODE(
        globalReadLock.emplace(opCtx, Date_t::now(), Lock::InterruptBehavior::kThrow),
        AssertionException,
        ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, GlobalLockInInterruptedContextThrowsEvenAcquiringRecursively) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::GlobalWrite globalWriteLock(opCtx, Date_t::now(), Lock::InterruptBehavior::kThrow);

    opCtx->markKilled();

    {
        boost::optional<Lock::GlobalWrite> recursiveGlobalWriteLock;
        ASSERT_THROWS_CODE(
            recursiveGlobalWriteLock.emplace(opCtx, Date_t::now(), Lock::InterruptBehavior::kThrow),
            AssertionException,
            ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture, GlobalLockInInterruptedContextRespectsUninterruptibleGuard) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    Lock::GlobalRead globalReadLock(
        opCtx, Date_t::now(), Lock::InterruptBehavior::kThrow);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextThrowsEvenWhenUncontested) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    boost::optional<Lock::DBLock> dbWriteLock;
    ASSERT_THROWS_CODE(dbWriteLock.emplace(opCtx, DatabaseName(boost::none, "db"), MODE_IX),
                       AssertionException,
                       ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextThrowsEvenWhenAcquiringRecursively) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbWriteLock(opCtx, DatabaseName(boost::none, "db"), MODE_X);

    opCtx->markKilled();

    {
        boost::optional<Lock::DBLock> recursiveDBWriteLock;
        ASSERT_THROWS_CODE(
            recursiveDBWriteLock.emplace(opCtx, DatabaseName(boost::none, "db"), MODE_X),
            AssertionException,
            ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextRespectsUninterruptibleGuard) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    Lock::DBLock dbWriteLock(opCtx, DatabaseName(boost::none, "db"), MODE_X);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, DBLockTimeout) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();

    const Milliseconds timeoutMillis = Milliseconds(1500);

    Lock::DBLock L1(opctx1, DatabaseName(boost::none, "testdb"), MODE_X, Date_t::max());
    ASSERT(opctx1->lockState()->isDbLockedForMode("testdb"_sd, MODE_X));
    ASSERT(L1.isLocked());

    Date_t t1 = Date_t::now();
    ASSERT_THROWS_CODE(
        Lock::DBLock(
            opctx2, DatabaseName(boost::none, "testdb"), MODE_X, Date_t::now() + timeoutMillis),
        AssertionException,
        ErrorCodes::LockTimeout);
    Date_t t2 = Date_t::now();
    ASSERT_GTE(t2 - t1 + kMaxClockJitterMillis, Milliseconds(timeoutMillis));
}

TEST_F(DConcurrencyTestFixture, DBLockTimeoutDueToGlobalLock) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();

    const Milliseconds timeoutMillis = Milliseconds(1500);

    Lock::GlobalLock G1(opctx1, MODE_X);
    ASSERT(G1.isLocked());

    Date_t t1 = Date_t::now();
    ASSERT_THROWS_CODE(
        Lock::DBLock(
            opctx2, DatabaseName(boost::none, "testdb"), MODE_X, Date_t::now() + timeoutMillis),
        AssertionException,
        ErrorCodes::LockTimeout);
    Date_t t2 = Date_t::now();
    ASSERT_GTE(t2 - t1 + kMaxClockJitterMillis, Milliseconds(timeoutMillis));
}

TEST_F(DConcurrencyTestFixture, CollectionLockInInterruptedContextThrowsEvenWhenUncontested) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, "db"), MODE_IX);
    opCtx->markKilled();

    {
        boost::optional<Lock::CollectionLock> collLock;
        ASSERT_THROWS_CODE(collLock.emplace(opCtx, NamespaceString("db.coll"), MODE_IX),
                           AssertionException,
                           ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture,
       CollectionLockInInterruptedContextThrowsEvenWhenAcquiringRecursively) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, "db"), MODE_IX);
    Lock::CollectionLock collLock(opCtx, NamespaceString("db.coll"), MODE_IX);

    opCtx->markKilled();

    {
        boost::optional<Lock::CollectionLock> recursiveCollLock;
        ASSERT_THROWS_CODE(recursiveCollLock.emplace(opCtx, NamespaceString("db.coll"), MODE_X),
                           AssertionException,
                           ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture, CollectionLockInInterruptedContextRespectsUninterruptibleGuard) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, "db"), MODE_IX);

    opCtx->markKilled();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    Lock::CollectionLock collLock(opCtx, NamespaceString("db.coll"), MODE_IX);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, CollectionLockTimeout) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();

    const Milliseconds timeoutMillis = Milliseconds(1500);

    Lock::DBLock DBL1(opctx1, DatabaseName(boost::none, "testdb"), MODE_IX, Date_t::max());
    ASSERT(opctx1->lockState()->isDbLockedForMode("testdb"_sd, MODE_IX));
    Lock::CollectionLock CL1(opctx1, NamespaceString("testdb.test"), MODE_X, Date_t::max());
    ASSERT(opctx1->lockState()->isCollectionLockedForMode(NamespaceString("testdb.test"), MODE_X));

    Date_t t1 = Date_t::now();
    Lock::DBLock DBL2(opctx2, DatabaseName(boost::none, "testdb"), MODE_IX, Date_t::max());
    ASSERT(opctx2->lockState()->isDbLockedForMode("testdb"_sd, MODE_IX));
    ASSERT_THROWS_CODE(
        Lock::CollectionLock(
            opctx2, NamespaceString("testdb.test"), MODE_X, Date_t::now() + timeoutMillis),
        AssertionException,
        ErrorCodes::LockTimeout);
    Date_t t2 = Date_t::now();
    ASSERT_GTE(t2 - t1 + kMaxClockJitterMillis, Milliseconds(timeoutMillis));
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstWithSXIS) {
    // Currently, we are allowed to acquire IX and X lock modes for RSTL. To overcome it,
    // this fail point will allow the test to acquire RSTL in any lock modes.
    FailPointEnableBlock enableTestOnlyFlag("enableTestOnlyFlagforRSTL");

    auto clientOpctxPairs = makeKClientsWithLockers(3);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();

    // Build a queue of MODE_S <- MODE_X <- MODE_IS, with MODE_S granted.
    repl::ReplicationStateTransitionLockGuard lockS(opctx1, MODE_S);
    ASSERT(lockS.isLocked());

    repl::ReplicationStateTransitionLockGuard lockX(
        opctx2, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockX.isLocked());

    // A MODE_IS should be granted due to compatibleFirst policy.
    repl::ReplicationStateTransitionLockGuard lockIS(opctx3, MODE_IS);
    ASSERT(lockIS.isLocked());

    ASSERT_THROWS_CODE(
        lockX.waitForLockUntil(Date_t::now()), AssertionException, ErrorCodes::LockTimeout);
    ASSERT(!lockX.isLocked());
}


TEST_F(DConcurrencyTestFixture, CompatibleFirstWithXSIXIS) {
    // Currently, we are allowed to acquire IX and X lock modes for RSTL. To overcome it,
    // this fail point will allow the test to acquire RSTL in any lock modes.
    FailPointEnableBlock enableTestOnlyFlag("enableTestOnlyFlagforRSTL");

    auto clientOpctxPairs = makeKClientsWithLockers(4);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_IX <- MODE_IS, with MODE_X granted.
    boost::optional<repl::ReplicationStateTransitionLockGuard> lockX;
    lockX.emplace(opctx1, MODE_X);
    ASSERT(lockX->isLocked());
    boost::optional<repl::ReplicationStateTransitionLockGuard> lockS;
    lockS.emplace(opctx2, MODE_S, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockS->isLocked());
    repl::ReplicationStateTransitionLockGuard lockIX(
        opctx3, MODE_IX, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    repl::ReplicationStateTransitionLockGuard lockIS(
        opctx4, MODE_IS, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the MODE_X and ensure that MODE_S will switch policy to compatibleFirst
    lockX.reset();
    lockS->waitForLockUntil(Date_t::now());
    ASSERT(lockS->isLocked());
    ASSERT(!lockIX.isLocked());
    lockIS.waitForLockUntil(Date_t::now());
    ASSERT(lockIS.isLocked());

    // Now release the MODE_S and ensure that MODE_IX gets locked.
    lockS.reset();
    lockIX.waitForLockUntil(Date_t::now());
    ASSERT(lockIX.isLocked());
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstWithXSXIXIS) {
    // Currently, we are allowed to acquire IX and X lock modes for RSTL. To overcome it,
    // this fail point will allow the test to acquire RSTL in any lock modes.
    FailPointEnableBlock enableTestOnlyFlag("enableTestOnlyFlagforRSTL");

    auto clientOpctxPairs = makeKClientsWithLockers(5);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();
    auto opctx5 = clientOpctxPairs[4].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_X <- MODE_IX <- MODE_IS, with the first MODE_X
    // granted and check that releasing it will result in the MODE_IS being granted.
    boost::optional<repl::ReplicationStateTransitionLockGuard> lockXgranted;
    lockXgranted.emplace(opctx1, MODE_X);
    ASSERT(lockXgranted->isLocked());

    boost::optional<repl::ReplicationStateTransitionLockGuard> lockX;
    lockX.emplace(opctx3, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockX->isLocked());

    // Now request MODE_S: it will be first in the pending list due to EnqueueAtFront policy.
    boost::optional<repl::ReplicationStateTransitionLockGuard> lockS;
    lockS.emplace(opctx2, MODE_S, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockS->isLocked());

    repl::ReplicationStateTransitionLockGuard lockIX(
        opctx4, MODE_IX, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    repl::ReplicationStateTransitionLockGuard lockIS(
        opctx5, MODE_IS, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the granted MODE_X and ensure that MODE_S will switch policy to compatibleFirst,
    // not locking the MODE_X or MODE_IX, but instead granting the final MODE_IS.
    lockXgranted.reset();
    lockS->waitForLockUntil(Date_t::now());
    ASSERT(lockS->isLocked());

    ASSERT_THROWS_CODE(
        lockX->waitForLockUntil(Date_t::now()), AssertionException, ErrorCodes::LockTimeout);
    ASSERT(!lockX->isLocked());
    ASSERT_THROWS_CODE(
        lockIX.waitForLockUntil(Date_t::now()), AssertionException, ErrorCodes::LockTimeout);
    ASSERT(!lockIX.isLocked());

    lockIS.waitForLockUntil(Date_t::now());
    ASSERT(lockIS.isLocked());
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstStress) {
    int numThreads = 8;
    int testMicros = 500'000;
    AtomicWord<unsigned long long> readOnlyInterval{0};
    AtomicWord<bool> done{false};
    std::vector<uint64_t> acquisitionCount(numThreads);
    std::vector<uint64_t> timeoutCount(numThreads);
    std::vector<uint64_t> busyWaitCount(numThreads);
    auto clientOpctxPairs = makeKClientsWithLockers(numThreads);

    // Do some busy waiting to trigger different timings. The atomic load prevents compilers
    // from optimizing the loop away.
    auto busyWait = [&done, &busyWaitCount](int threadId, long long iters) {
        while (iters-- > 0) {
            for (int i = 0; i < 100 && !done.load(); i++) {
                busyWaitCount[threadId]++;
            }
        }
    };

    std::vector<stdx::thread> threads;

    // Thread putting state in/out of read-only CompatibleFirst mode.
    threads.emplace_back([&]() {
        Timer t;
        auto endTime = t.micros() + testMicros;
        uint64_t readOnlyIntervalCount = 0;
        OperationContext* opCtx = clientOpctxPairs[0].second.get();
        for (int iters = 0; (t.micros() < endTime); iters++) {
            busyWait(0, iters % 20);
            Lock::GlobalRead readLock(opCtx,
                                      Date_t::now() + Milliseconds(iters % 2),
                                      Lock::InterruptBehavior::kLeaveUnlocked);
            if (!readLock.isLocked()) {
                timeoutCount[0]++;
                continue;
            }
            acquisitionCount[0]++;
            readOnlyInterval.store(++readOnlyIntervalCount);
            busyWait(0, iters % 200);
            readOnlyInterval.store(0);
        };
        done.store(true);
    });

    for (int threadId = 1; threadId < numThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            Timer t;
            for (int iters = 0; !done.load(); iters++) {
                OperationContext* opCtx = clientOpctxPairs[threadId].second.get();
                boost::optional<Lock::GlobalLock> lock;
                switch (threadId) {
                    case 1:
                    case 2:
                    case 3:
                    case 4: {
                        // Here, actually try to acquire a lock without waiting, and check whether
                        // we should have gotten the lock or not. Use MODE_IS in 95% of the cases,
                        // and MODE_S in only 5, as that stressing the partitioning scheme and
                        // policy changes more as thread 0 acquires/releases its MODE_S lock.
                        busyWait(threadId, iters % 100);
                        auto interval = readOnlyInterval.load();
                        lock.emplace(opCtx,
                                     iters % 20 ? MODE_IS : MODE_S,
                                     Date_t::now(),
                                     Lock::InterruptBehavior::kLeaveUnlocked);
                        // If thread 0 is holding the MODE_S lock while we tried to acquire a
                        // MODE_IS or MODE_S lock, the CompatibleFirst policy guarantees success.
                        auto newInterval = readOnlyInterval.load();
                        invariant(!interval || interval != newInterval || lock->isLocked());
                        break;
                    }
                    case 5:
                        busyWait(threadId, iters % 150);
                        lock.emplace(opCtx,
                                     MODE_X,
                                     Date_t::now() + Milliseconds(iters % 2),
                                     Lock::InterruptBehavior::kLeaveUnlocked);
                        busyWait(threadId, iters % 10);
                        break;
                    case 6:
                        lock.emplace(opCtx,
                                     iters % 25 ? MODE_IX : MODE_S,
                                     Date_t::now() + Milliseconds(iters % 2),
                                     Lock::InterruptBehavior::kLeaveUnlocked);
                        busyWait(threadId, iters % 100);
                        break;
                    case 7:
                        busyWait(threadId, iters % 100);
                        lock.emplace(opCtx,
                                     iters % 20 ? MODE_IS : MODE_X,
                                     Date_t::now(),
                                     Lock::InterruptBehavior::kLeaveUnlocked);
                        break;
                    default:
                        MONGO_UNREACHABLE;
                }
                if (lock->isLocked())
                    acquisitionCount[threadId]++;
                else
                    timeoutCount[threadId]++;
            };
        });
    }

    for (auto& thread : threads)
        thread.join();
    for (int threadId = 0; threadId < numThreads; threadId++) {
        LOGV2(20515,
              "thread {threadId} stats: {acquisitionCount_threadId} acquisitions, "
              "{timeoutCount_threadId} timeouts, {busyWaitCount_threadId_1_000_000}M busy waits",
              "threadId"_attr = threadId,
              "acquisitionCount_threadId"_attr = acquisitionCount[threadId],
              "timeoutCount_threadId"_attr = timeoutCount[threadId],
              "busyWaitCount_threadId_1_000_000"_attr = busyWaitCount[threadId] / 1'000'000);
    }
}


namespace {
class RecoveryUnitMock : public RecoveryUnitNoop {
public:
    bool activeTransaction = true;

private:
    void doAbandonSnapshot() override {
        activeTransaction = false;
    }
};
}  // namespace

TEST_F(DConcurrencyTestFixture, TestGlobalLockAbandonsSnapshotWhenNotInWriteUnitOfWork) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    auto recovUnitOwned = std::make_unique<RecoveryUnitMock>();
    auto recovUnitBorrowed = recovUnitOwned.get();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(recovUnitOwned.release()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    {
        Lock::GlobalLock gw1(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(gw1.isLocked());
        ASSERT(recovUnitBorrowed->activeTransaction);

        {
            Lock::GlobalLock gw2(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
            ASSERT(gw2.isLocked());
            ASSERT(recovUnitBorrowed->activeTransaction);
        }

        ASSERT(recovUnitBorrowed->activeTransaction);
        ASSERT(gw1.isLocked());
    }
    ASSERT_FALSE(recovUnitBorrowed->activeTransaction);
}

TEST_F(DConcurrencyTestFixture, TestGlobalLockDoesNotAbandonSnapshotWhenInWriteUnitOfWork) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    auto recovUnitOwned = std::make_unique<RecoveryUnitMock>();
    auto recovUnitBorrowed = recovUnitOwned.get();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(recovUnitOwned.release()),
                           WriteUnitOfWork::RecoveryUnitState::kActiveUnitOfWork);
    opCtx->lockState()->beginWriteUnitOfWork();

    {
        Lock::GlobalLock gw1(opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(gw1.isLocked());
        ASSERT(recovUnitBorrowed->activeTransaction);

        {
            Lock::GlobalLock gw2(opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
            ASSERT(gw2.isLocked());
            ASSERT(recovUnitBorrowed->activeTransaction);
        }

        ASSERT(recovUnitBorrowed->activeTransaction);
        ASSERT(gw1.isLocked());
    }
    ASSERT_TRUE(recovUnitBorrowed->activeTransaction);

    opCtx->lockState()->endWriteUnitOfWork();
}

TEST_F(DConcurrencyTestFixture, RSTLLockGuardTimeout) {
    auto clients = makeKClientsWithLockers(2);
    auto firstOpCtx = clients[0].second.get();
    auto secondOpCtx = clients[1].second.get();

    // The first opCtx holds the RSTL.
    repl::ReplicationStateTransitionLockGuard firstRSTL(firstOpCtx, MODE_X);
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_EQ(firstOpCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);

    // The second opCtx enqueues the lock request but cannot acquire it.
    repl::ReplicationStateTransitionLockGuard secondRSTL(
        secondOpCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT_FALSE(secondRSTL.isLocked());

    // The second opCtx times out.
    ASSERT_THROWS_CODE(secondRSTL.waitForLockUntil(Date_t::now() + Milliseconds(1)),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // Check the first opCtx is still holding the RSTL.
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_EQ(firstOpCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_FALSE(secondRSTL.isLocked());
}

TEST_F(DConcurrencyTestFixture, RSTLLockGuardEnqueueAndWait) {
    auto clients = makeKClientsWithLockers(2);
    auto firstOpCtx = clients[0].second.get();
    auto secondOpCtx = clients[1].second.get();

    // The first opCtx holds the RSTL.
    auto firstRSTL =
        std::make_unique<repl::ReplicationStateTransitionLockGuard>(firstOpCtx, MODE_X);
    ASSERT_TRUE(firstRSTL->isLocked());
    ASSERT_EQ(firstOpCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);


    // The second opCtx enqueues the lock request but cannot acquire it.
    repl::ReplicationStateTransitionLockGuard secondRSTL(
        secondOpCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT_FALSE(secondRSTL.isLocked());

    // The first opCtx unlocks so the second opCtx acquires it.
    firstRSTL.reset();
    ASSERT_EQ(firstOpCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);


    secondRSTL.waitForLockUntil(Date_t::now());
    ASSERT_TRUE(secondRSTL.isLocked());
    ASSERT_EQ(secondOpCtx->lockState()->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);
}

TEST_F(DConcurrencyTestFixture, RSTLLockGuardResilientToExceptionThrownBeforeWaitForRSTLComplete) {
    auto clients = makeKClientsWithLockers(2);
    auto firstOpCtx = clients[0].second.get();
    auto secondOpCtx = clients[1].second.get();

    // The first opCtx holds the RSTL.
    repl::ReplicationStateTransitionLockGuard firstRSTL(firstOpCtx, MODE_X);
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_TRUE(firstOpCtx->lockState()->isRSTLExclusive());

    {
        // The second opCtx enqueues the lock request but cannot acquire it.
        repl::ReplicationStateTransitionLockGuard secondRSTL(
            secondOpCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
        ASSERT_FALSE(secondRSTL.isLocked());

        // secondRSTL is going to go out of scope with the lock result as LOCK_WAITING. As a result,
        // ReplicationStateTransitionLockGuard destructor will be called and we should expect
        // the RSTL lock state cleaned from both locker and lock manager.
    }

    // Verify that the RSTL lock state is cleaned from secondOpCtx's locker.
    ASSERT_FALSE(secondOpCtx->lockState()->isRSTLLocked());

    // Now, make first opCtx to release the lock to test if we can reacquire the lock again.
    ASSERT_TRUE(firstOpCtx->lockState()->isRSTLExclusive());
    firstRSTL.release();
    ASSERT_FALSE(firstRSTL.isLocked());

    // If we haven't cleaned the RSTL lock state from the conflict queue in the lock manager after
    // the destruction of secondRSTL, first opCtx won't be able to reacquire the RSTL in X mode.
    firstRSTL.reacquire();
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_TRUE(firstOpCtx->lockState()->isRSTLExclusive());
}

TEST_F(DConcurrencyTestFixture, FailPointInLockDoesNotFailUninterruptibleGlobalNonIntentLocks) {
    auto opCtx = makeOperationContext();

    FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

    LockerImpl locker1(opCtx->getServiceContext());
    LockerImpl locker2(opCtx->getServiceContext());
    LockerImpl locker3(opCtx->getServiceContext());

    {
        locker1.lockGlobal(opCtx.get(), MODE_IX);

        // MODE_S attempt.
        stdx::thread t2([&]() {
            UninterruptibleLockGuard noInterrupt(&locker2);
            locker2.lockGlobal(opCtx.get(), MODE_S);
        });

        // Wait for the thread to attempt to acquire the global lock in MODE_S.
        waitForLockerToHaveWaitingResource(&locker2);

        locker1.unlockGlobal();
        t2.join();
        locker2.unlockGlobal();
    }

    {
        locker1.lockGlobal(opCtx.get(), MODE_IX);

        // MODE_X attempt.
        stdx::thread t3([&]() {
            UninterruptibleLockGuard noInterrupt(&locker3);
            locker3.lockGlobal(opCtx.get(), MODE_X);
        });

        // Wait for the thread to attempt to acquire the global lock in MODE_X.
        waitForLockerToHaveWaitingResource(&locker3);

        locker1.unlockGlobal();
        t3.join();
        locker3.unlockGlobal();
    }
}

TEST_F(DConcurrencyTestFixture, FailPointInLockDoesNotFailUninterruptibleNonIntentLocks) {
    auto opCtx = makeOperationContext();

    FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

    LockerImpl locker1(opCtx->getServiceContext());
    LockerImpl locker2(opCtx->getServiceContext());
    LockerImpl locker3(opCtx->getServiceContext());

    // Granted MODE_X lock, fail incoming MODE_S and MODE_X.
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    locker1.lockGlobal(opCtx.get(), MODE_IX);

    {
        locker1.lock(resId, MODE_X);

        // MODE_S attempt.
        stdx::thread t2([&]() {
            UninterruptibleLockGuard noInterrupt(&locker2);
            locker2.lockGlobal(opCtx.get(), MODE_IS);
            locker2.lock(resId, MODE_S);
        });

        // Wait for the thread to attempt to acquire the collection lock in MODE_S.
        waitForLockerToHaveWaitingResource(&locker2);

        locker1.unlock(resId);
        t2.join();
        locker2.unlock(resId);
        locker2.unlockGlobal();
    }

    {
        locker1.lock(resId, MODE_X);

        // MODE_X attempt.
        stdx::thread t3([&]() {
            UninterruptibleLockGuard noInterrupt(&locker3);
            locker3.lockGlobal(opCtx.get(), MODE_IX);
            locker3.lock(resId, MODE_X);
        });

        // Wait for the thread to attempt to acquire the collection lock in MODE_X.
        waitForLockerToHaveWaitingResource(&locker3);

        locker1.unlock(resId);
        t3.join();
        locker3.unlock(resId);
        locker3.unlockGlobal();
    }

    locker1.unlockGlobal();
}

TEST_F(DConcurrencyTestFixture, PBWMRespectsMaxTimeMS) {
    auto clientOpCtxPairs = makeKClientsWithLockers(2);
    auto opCtx1 = clientOpCtxPairs[0].second.get();
    auto opCtx2 = clientOpCtxPairs[1].second.get();

    Lock::ResourceLock pbwm1(opCtx1->lockState(), resourceIdParallelBatchWriterMode);
    pbwm1.lock(nullptr, MODE_X);

    opCtx2->setDeadlineAfterNowBy(Seconds{1}, ErrorCodes::ExceededTimeLimit);

    Lock::ResourceLock pbwm2(opCtx2->lockState(), resourceIdParallelBatchWriterMode);

    ASSERT_THROWS_CODE(
        pbwm2.lock(opCtx2, MODE_X), AssertionException, ErrorCodes::ExceededTimeLimit);
}

TEST_F(DConcurrencyTestFixture, DifferentTenantsTakeDBLockOnConflictingNamespaceOk) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto db = "db1";
    auto tenant1 = TenantId(OID::gen());
    auto tenant2 = TenantId(OID::gen());

    DatabaseName dbName1(tenant1, db);
    DatabaseName dbName2(tenant2, db);

    Lock::DBLock r1(opCtx1, dbName1, MODE_X);
    Lock::DBLock r2(opCtx2, dbName2, MODE_X);

    ASSERT(opCtx1->lockState()->isDbLockedForMode(dbName1.toStringWithTenantId(), MODE_X));
    ASSERT(opCtx2->lockState()->isDbLockedForMode(dbName2.toStringWithTenantId(), MODE_X));
}

TEST_F(DConcurrencyTestFixture, ConflictingTenantDBLockThrows) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto db = "db1";
    DatabaseName dbName1(TenantId(OID::gen()), db);

    Lock::DBLock r1(opCtx1, dbName1, MODE_X);

    auto result = runTaskAndKill(opCtx2, [&]() { Lock::DBLock r2(opCtx2, dbName1, MODE_S); });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
}

}  // namespace
}  // namespace mongo
