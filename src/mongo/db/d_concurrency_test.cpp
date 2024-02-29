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

// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/execution_control/concurrency_adjustment_parameters_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/ticketholder_manager.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

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
 * acquisitions to numTickets. The opCtx must live as long as the UseReaderWriterGlobalThrottling
 * instance.
 */
template <class TicketHolderImpl>
class UseReaderWriterGlobalThrottling {
public:
    explicit UseReaderWriterGlobalThrottling(ServiceContext* svcCtx, int numTickets)
        : _svcCtx(svcCtx) {
        const bool trackPeakUsed = false;
        // TODO SERVER-72616: Remove ifdefs once PriorityTicketHolder is available cross-platform.
#ifdef __linux__
        if constexpr (std::is_same_v<PriorityTicketHolder, TicketHolderImpl>) {
            LOGV2(7130100, "Using PriorityTicketHolder for Reader/Writer global throttling");
            // For simplicity, no low priority operations will ever be expedited in these tests.
            auto lowPriorityAdmissionsBypassThreshold = 0;

            auto ticketHolderManager = std::make_unique<FixedTicketHolderManager>(
                std::make_unique<PriorityTicketHolder>(
                    _svcCtx, numTickets, lowPriorityAdmissionsBypassThreshold, trackPeakUsed),
                std::make_unique<PriorityTicketHolder>(
                    _svcCtx, numTickets, lowPriorityAdmissionsBypassThreshold, trackPeakUsed));
            TicketHolderManager::use(_svcCtx, std::move(ticketHolderManager));
        } else {
            LOGV2(7130101, "Using SemaphoreTicketHolder for Reader/Writer global throttling");
            auto ticketHolderManager = std::make_unique<FixedTicketHolderManager>(
                std::make_unique<SemaphoreTicketHolder>(_svcCtx, numTickets, trackPeakUsed),
                std::make_unique<SemaphoreTicketHolder>(_svcCtx, numTickets, trackPeakUsed));
            TicketHolderManager::use(_svcCtx, std::move(ticketHolderManager));
        }
#else
        LOGV2(7207205, "Using SemaphoreTicketHolder for Reader/Writer global throttling");
        auto ticketHolderManager = std::make_unique<FixedTicketHolderManager>(
            std::make_unique<SemaphoreTicketHolder>(_svcCtx, numTickets, trackPeakUsed),
            std::make_unique<SemaphoreTicketHolder>(_svcCtx, numTickets, trackPeakUsed));
        TicketHolderManager::use(_svcCtx, std::move(ticketHolderManager));
#endif
    }

    ~UseReaderWriterGlobalThrottling() noexcept(false) {
        TicketHolderManager::use(_svcCtx, nullptr);
    }

private:
    ServiceContext* _svcCtx;
};


class DConcurrencyTestFixture : public ServiceContextTest {
public:
    ServiceContext::UniqueOperationContext makeOperationContextWithLocker() {
        auto opCtx = makeOperationContext();
        return opCtx;
    }

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeKClientsWithLockers(int k) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(k);
        for (int i = 0; i < k; ++i) {
            auto client = getServiceContext()->getService()->makeClient(
                str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
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

    template <class TicketHolderImpl, class F>
    void runWithThrottling(int numTickets, F&& f) {
        // TODO SERVER-72616: Remove ifdef when PriorityTicketHolder is available cross-platform.
#ifdef __linux__
        UseReaderWriterGlobalThrottling<TicketHolderImpl> throttle(getServiceContext(), numTickets);
        f();
#else
        // We can only test non-PriorityTicketHolder implementations on non-Linux platforms.
        if constexpr (!std::is_same_v<PriorityTicketHolder, TicketHolderImpl>) {
            UseReaderWriterGlobalThrottling<TicketHolderImpl> throttle(getServiceContext(),
                                                                       numTickets);
            f();
        }
#endif
    }
};


TEST_F(DConcurrencyTestFixture, WriteConflictRetryInstantiatesOK) {
    auto opCtx = makeOperationContextWithLocker();
    writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [] {});
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryRetriesFunctionOnWriteConflictException) {
    auto opCtx = makeOperationContextWithLocker();
    auto&& opDebug = CurOp::get(opCtx.get())->debug();
    ASSERT_EQUALS(0, opDebug.additiveMetrics.writeConflicts.load());
    ASSERT_EQUALS(100, writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [&opDebug] {
                      if (0 == opDebug.additiveMetrics.writeConflicts.load()) {
                          throwWriteConflictException(
                              str::stream()
                              << "Verify that we retry the WriteConflictRetry function when we "
                                 "encounter a WriteConflictException.");
                      }
                      return 100;
                  }));
    ASSERT_EQUALS(1LL, opDebug.additiveMetrics.writeConflicts.load());
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryPropagatesNonWriteConflictException) {
    auto opCtx = makeOperationContextWithLocker();
    ASSERT_THROWS_CODE(writeConflictRetry(opCtx.get(),
                                          "",
                                          NamespaceString::kEmpty,
                                          [] {
                                              uassert(ErrorCodes::OperationFailed, "", false);
                                              MONGO_UNREACHABLE;
                                          }),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

TEST_F(DConcurrencyTestFixture,
       WriteConflictRetryPropagatesWriteConflictExceptionIfAlreadyInAWriteUnitOfWork) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::GlobalWrite globalWrite(opCtx.get());
    WriteUnitOfWork wuow(opCtx.get());
    ASSERT_THROWS(writeConflictRetry(
                      opCtx.get(),
                      "",
                      NamespaceString::kEmpty,
                      [] {
                          throwWriteConflictException(
                              str::stream() << "Verify that WriteConflictExceptions are propogated "
                                               "if we are already in a WriteUnitOfWork.");
                      }),
                  WriteConflictException);
}

TEST_F(DConcurrencyTestFixture, ResourceMutex) {
    Lock::ResourceMutex mtx("testMutex");
    auto opCtx = makeOperationContext();
    auto clients = makeKClientsWithLockers(3);

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
        boost::optional<Lock::SharedLock> lk;

        // Step 0: Single thread acquires shared lock
        state.waitFor(0);
        lk.emplace(clients[0].second.get(), mtx);
        state.finish(0);

        // Step 4: Wait for t2 to regain its shared lock
        {
            state.waitFor(4);
            state.waitFor([locker1 = shard_role_details::getLocker(clients[1].second.get())]() {
                return locker1->getWaitingResource().isValid();
            });
            state.finish(4);
        }

        // Step 5: After t2 becomes blocked, unlock, yielding the mutex to t3
        lk.reset();
    });
    stdx::thread t2([&]() {
        boost::optional<Lock::SharedLock> lk;

        // Step 1: Two threads acquire shared lock
        state.waitFor(1);
        lk.emplace(clients[1].second.get(), mtx);
        state.finish(1);

        // Step 2: Wait for t3 to attempt the exclusive lock
        state.waitFor([locker2 = shard_role_details::getLocker(clients[2].second.get())]() {
            return locker2->getWaitingResource().isValid();
        });
        state.finish(2);

        // Step 3: Yield shared lock
        lk.reset();
        state.finish(3);

        // Step 4: Try to regain the shared lock // transfers control to t1
        lk.emplace(clients[1].second.get(), mtx);

        // Step 6: Check we actually got back the shared lock
        state.check(6);
    });
    stdx::thread t3([&]() {
        // Step 2: Third thread attempts to acquire exclusive lock
        state.waitFor(2);

        // Step 5: Actually get the exclusive lock
        Lock::ExclusiveLock lk(clients[2].second.get(),
                               mtx);  // transfers control to t2
        state.finish(5);
    });
    t1.join();
    t2.join();
    t3.join();
}

TEST_F(DConcurrencyTestFixture, GlobalRead) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::GlobalRead globalRead(opCtx.get());
    ASSERT(shard_role_details::getLocker(opCtx.get())->isR());
    ASSERT_EQ(shard_role_details::getLocker(opCtx.get())
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_IX);
}

TEST_F(DConcurrencyTestFixture, GlobalWrite) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(shard_role_details::getLocker(opCtx.get())->isW());
    ASSERT_EQ(shard_role_details::getLocker(opCtx.get())
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_IX);
}

TEST_F(DConcurrencyTestFixture, GlobalWriteAndGlobalRead) {
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

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
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

    auto globalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(
            opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
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
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

    auto globalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(resourceIdGlobal))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(resourceIdGlobal));
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(
            opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
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
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

    auto outerGlobalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    auto innerGlobalWrite = std::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT_EQ(lockState->getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    {
        Lock::DBLock dbWrite(
            opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
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
        clients[0].second.get(), resourceIdReplicationStateTransitionLock, MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(clients[0].second.get())
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);

    ASSERT_THROWS_CODE(Lock::GlobalLock(clients[1].second.get(),
                                        MODE_X,
                                        Date_t::now() + Milliseconds(1),
                                        Lock::InterruptBehavior::kThrow),
                       AssertionException,
                       ErrorCodes::LockTimeout);
    ASSERT_EQ(shard_role_details::getLocker(clients[0].second.get())
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(clients[1].second.get())
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());

    {
        Lock::GlobalLock globalWrite(opCtx, MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockIXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalWrite(
            opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockSDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_S, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockISDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockXSetsGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());

    {
        Lock::DBLock dbWrite(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X);
    }
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockSDoesNotSetGlobalWriteLockedOnOperationContext) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());

    {
        Lock::DBLock dbRead(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_S);
    }
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, TenantLock) {
    auto opCtx = makeOperationContextWithLocker();
    TenantId tenantId{OID::gen()};
    ResourceId tenantResourceId{ResourceType::RESOURCE_TENANT, tenantId};
    struct TestCase {
        LockMode globalLockMode;
        LockMode tenantLockMode;
    };
    std::vector<TestCase> testCases{
        {MODE_IX, MODE_IX}, {MODE_IX, MODE_X}, {MODE_IS, MODE_S}, {MODE_IS, MODE_IS}};
    for (auto&& testCase : testCases) {
        {
            Lock::GlobalLock globalLock{opCtx.get(), testCase.globalLockMode};
            Lock::TenantLock tenantLock{opCtx.get(), tenantId, testCase.tenantLockMode};
            ASSERT_TRUE(shard_role_details::getLocker(opCtx.get())
                            ->isLockHeldForMode(tenantResourceId, testCase.tenantLockMode));
        }
        ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())
                         ->isLockHeldForMode(tenantResourceId, testCase.tenantLockMode));
    }
}

TEST_F(DConcurrencyTestFixture, DBLockTakesTenantLock) {
    auto opCtx = makeOperationContextWithLocker();
    TenantId tenantId{OID::gen()};
    ResourceId tenantResourceId{ResourceType::RESOURCE_TENANT, tenantId};
    struct TestCase {
        bool tenantOwned;
        LockMode databaseLockMode;
        boost::optional<LockMode> tenantLockMode;
        LockMode expectedTenantLockMode;
    };

    StringData testDatabaseName{"test"};
    const bool tenantOwned{true};
    const bool tenantless{false};
    const boost::optional<LockMode> none;
    std::vector<TestCase> testCases{
        {tenantless, MODE_S, none, MODE_NONE},
        {tenantless, MODE_IS, none, MODE_NONE},
        {tenantless, MODE_X, none, MODE_NONE},
        {tenantless, MODE_IX, none, MODE_NONE},
        {tenantOwned, MODE_S, none, MODE_IS},
        {tenantOwned, MODE_IS, none, MODE_IS},
        {tenantOwned, MODE_X, none, MODE_IX},
        {tenantOwned, MODE_IX, none, MODE_IX},
        {tenantOwned, MODE_X, MODE_X, MODE_X},
        {tenantOwned, MODE_IX, MODE_X, MODE_X},
    };
    for (auto&& testCase : testCases) {
        {
            Lock::DBLock dbLock(
                opCtx.get(),
                DatabaseName::createDatabaseName_forTest(
                    testCase.tenantOwned ? boost::make_optional(tenantId) : boost::none,
                    testDatabaseName),
                testCase.databaseLockMode,
                Date_t::max(),
                testCase.tenantLockMode);
            ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(tenantResourceId) ==
                   testCase.expectedTenantLockMode)
                << " db lock mode: " << modeName(testCase.databaseLockMode)
                << ", tenant lock mode: "
                << (testCase.tenantLockMode ? modeName(*testCase.tenantLockMode) : "-");
        }
        ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(tenantResourceId) ==
               MODE_NONE)
            << " db lock mode: " << modeName(testCase.databaseLockMode) << ", tenant lock mode: "
            << (testCase.tenantLockMode ? modeName(*testCase.tenantLockMode) : "-");
    }

    // Verify that tenant lock survives move.
    {
        auto lockBuilder = [&]() {
            return Lock::DBLock{
                opCtx.get(),
                DatabaseName::createDatabaseName_forTest(tenantId, testDatabaseName),
                MODE_S};
        };
        Lock::DBLock dbLockCopy{lockBuilder()};
        ASSERT(shard_role_details::getLocker(opCtx.get())
                   ->isLockHeldForMode(tenantResourceId, MODE_IS));
    }
}

TEST_F(DConcurrencyTestFixture, GlobalLockXDoesNotSetGlobalWriteLockedWhenLockAcquisitionTimesOut) {
    auto clients = makeKClientsWithLockers(2);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = clients[1].second.get();
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
    {
        ASSERT_THROWS_CODE(
            Lock::GlobalLock(
                opCtx, MODE_X, Date_t::now() + Milliseconds(1), Lock::InterruptBehavior::kThrow),
            AssertionException,
            ErrorCodes::LockTimeout);
    }
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->wasGlobalLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockSSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::GlobalLock globalWrite(opCtx, MODE_S, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockISDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockIXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, GlobalLockXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockSDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::DBLock dbWrite(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_S);
    }
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockISDoesNotSetGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::DBLock dbWrite(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IS);
    }
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockIXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::DBLock dbWrite(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
    }
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture, DBLockXSetsGlobalLockTakenInModeConflictingWithWrites) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());

    {
        Lock::DBLock dbRead(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X);
    }
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
}

TEST_F(DConcurrencyTestFixture,
       GlobalLockSDoesNotSetGlobalLockTakenInModeConflictingWithWritesWhenLockAcquisitionTimesOut) {
    auto clients = makeKClientsWithLockers(2);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(
        clients[0].second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = clients[1].second.get();
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
    {
        ASSERT_THROWS_CODE(
            Lock::GlobalLock(
                opCtx, MODE_X, Date_t::now() + Milliseconds(1), Lock::InterruptBehavior::kThrow),
            AssertionException,
            ErrorCodes::LockTimeout);
    }
    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
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
    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, GlobalLockWaitIsInterruptibleBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    Lock::ResourceLock rstl(opCtx1, resourceIdReplicationStateTransitionLock, MODE_X);

    auto result = runTaskAndKill(opCtx2, [&]() {
        // Killing the lock wait should throw an exception.
        Lock::GlobalLock g(opCtx2, MODE_S);
    });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
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
    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
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
    Lock::ResourceLock rstl(opCtx1, resourceIdReplicationStateTransitionLock, MODE_X);
    // Acquire this later to confirm that it stays unlocked.
    boost::optional<Lock::GlobalLock> g2 = boost::none;

    // Killing the lock wait should not interrupt it, but rather leave it lock unlocked.
    auto result = runTaskAndKill(opCtx2, [&]() {
        g2.emplace(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);
    });
    ASSERT(g2 != boost::none);
    ASSERT(!g2->isLocked());
    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
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
    shard_role_details::getLocker(opCtx2)->setMaxLockTimeout(Milliseconds(100));
    Lock::GlobalLock g2(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);

    ASSERT(g1.isLocked());
    ASSERT(!g2.isLocked());

    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, SetMaxLockTimeoutMillisAndNotUsingInterruptBehaviorBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::ResourceLock rstl(opCtx1, resourceIdReplicationStateTransitionLock, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown
    // and then caught by the Lock::InterruptBehavior::kLeaveUnlocked setting.
    shard_role_details::getLocker(opCtx2)->setMaxLockTimeout(Milliseconds(100));
    Lock::GlobalLock g2(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kLeaveUnlocked);

    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
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
    shard_role_details::getLocker(opCtx2)->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT(g1.isLocked());

    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture,
       SetMaxLockTimeoutMillisAndThrowUsingInterruptBehaviorBlockedOnRSTL) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // Take the exclusive lock with the first caller.
    Lock::ResourceLock rstl(opCtx1, resourceIdReplicationStateTransitionLock, MODE_X);

    // Set a max timeout on the second caller that will override provided lock request deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown.
    shard_role_details::getLocker(opCtx2)->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_S, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_X);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, FailedGlobalLockShouldUnlockRSTLOnlyOnce) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto resourceRSTL = resourceIdReplicationStateTransitionLock;

    // Take the exclusive lock with the first caller.
    Lock::GlobalLock globalLock(opCtx1, MODE_X);

    shard_role_details::getLocker(opCtx2)->beginWriteUnitOfWork();
    // Set a max timeout on the second caller that will override provided lock request
    // deadlines.
    // Then requesting a lock with Date_t::max() should cause a LockTimeout error to be thrown.
    shard_role_details::getLocker(opCtx2)->setMaxLockTimeout(Milliseconds(100));

    ASSERT_THROWS_CODE(
        Lock::GlobalLock(opCtx2, MODE_IX, Date_t::max(), Lock::InterruptBehavior::kThrow),
        DBException,
        ErrorCodes::LockTimeout);
    auto opCtx2Locker = shard_role_details::getLocker(opCtx2);
    // GlobalLock failed, but the RSTL should be successfully acquired and pending unlocked.
    ASSERT(opCtx2Locker->getRequestsForTest().find(resourceIdGlobal).finished());
    ASSERT_EQ(opCtx2Locker->getRequestsForTest().find(resourceRSTL).objAddr()->unlockPending, 1U);
    ASSERT_EQ(opCtx2Locker->getRequestsForTest().find(resourceRSTL).objAddr()->recursiveCount, 1U);
    shard_role_details::getLocker(opCtx2)->endWriteUnitOfWork();
    ASSERT_EQ(shard_role_details::getLocker(opCtx1)->getLockMode(resourceRSTL), MODE_IX);
    ASSERT_EQ(shard_role_details::getLocker(opCtx2)->getLockMode(resourceRSTL), MODE_NONE);
}

TEST_F(DConcurrencyTestFixture, DBLockWaitIsInterruptible) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    // The main thread takes an exclusive lock, causing the spawned thread to wait when it attempts
    // to acquire a conflicting lock.
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db");
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
    auto result = runTaskAndKill(
        opCtx2,
        [&]() {
            UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx2));  // NOLINT.
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
        Lock::DBLock(opCtx1, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X);

    // Killing the lock wait should not interrupt it.
    auto result = runTaskAndKill(
        opCtx2,
        [&]() {
            UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx2));  // NOLINT.
            Lock::DBLock d(
                opCtx2, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_S);
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
    LockResult result = shard_role_details::getLocker(opCtx2)->lockRSTLBegin(opCtx2, MODE_X);
    ASSERT_EQ(result, LOCK_WAITING);

    // Release the conflicting lock.
    lockXGranted.reset();

    {
        stdx::lock_guard<Client> clientLock(*opCtx2->getClient());
        opCtx2->markKilled();
    }

    // After the operation has been killed, the lockComplete request should fail, even though the
    // lock is uncontested.
    ASSERT_THROWS_CODE(
        shard_role_details::getLocker(opCtx2)->lockRSTLComplete(opCtx2, MODE_X, Date_t::max()),
        AssertionException,
        ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesS) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbRead(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_S);

    const ResourceId resIdDb(RESOURCE_DATABASE,
                             DatabaseName::createDatabaseName_forTest(boost::none, "db"));
    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resIdDb) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesX) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbWrite(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X);

    const ResourceId resIdDb(RESOURCE_DATABASE,
                             DatabaseName::createDatabaseName_forTest(boost::none, "db"));
    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resIdDb) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesISForAdminIS) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbRead(opCtx.get(), DatabaseName::kAdmin, MODE_IS);

    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resourceIdAdminDB) == MODE_IS);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesSForAdminS) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbRead(opCtx.get(), DatabaseName::kAdmin, MODE_S);

    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resourceIdAdminDB) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesIXForAdminIX) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbWrite(opCtx.get(), DatabaseName::kAdmin, MODE_IX);

    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resourceIdAdminDB) == MODE_IX);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminX) {
    auto opCtx = makeOperationContextWithLocker();
    Lock::DBLock dbWrite(opCtx.get(), DatabaseName::kAdmin, MODE_X);

    ASSERT(shard_role_details::getLocker(opCtx.get())->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, MultipleWriteDBLocksOnSameThread) {
    auto opCtx = makeOperationContextWithLocker();
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    Lock::DBLock r1(opCtx.get(), dbName, MODE_X);
    Lock::DBLock r2(opCtx.get(), dbName, MODE_X);

    ASSERT(shard_role_details::getLocker(opCtx.get())->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, MultipleConflictingDBLocksOnSameThread) {
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    Lock::DBLock r1(opCtx.get(), dbName, MODE_X);
    Lock::DBLock r2(opCtx.get(), dbName, MODE_S);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_X));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForMode_IsCollectionLockedForMode) {
    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

    // Database ownership options to test.
    enum DatabaseOwnershipOptions {
        // Owned by a tenant and not.
        kAll,
        // Owned by a tenant only.
        kTenantOwned
    };
    struct TestCase {
        LockMode globalLockMode;
        LockMode tenantLockMode;
        DatabaseOwnershipOptions databaseOwnership;
        LockMode databaseLockMode;
        LockMode checkedDatabaseLockMode;
        bool expectedResult;
    };

    TenantId tenantId{OID::gen()};
    StringData testDatabaseName{"test"};
    std::vector<TestCase> testCases{
        // Only global lock acquired.
        {MODE_X, MODE_NONE, kAll, MODE_NONE, MODE_X, true},
        {MODE_X, MODE_NONE, kAll, MODE_NONE, MODE_IX, true},
        {MODE_X, MODE_NONE, kAll, MODE_NONE, MODE_S, true},
        {MODE_X, MODE_NONE, kAll, MODE_NONE, MODE_IS, true},
        {MODE_S, MODE_NONE, kAll, MODE_NONE, MODE_X, false},
        {MODE_S, MODE_NONE, kAll, MODE_NONE, MODE_IX, false},
        {MODE_S, MODE_NONE, kAll, MODE_NONE, MODE_S, true},
        {MODE_S, MODE_NONE, kAll, MODE_NONE, MODE_IS, true},
        // Global and tenant locks acquired.
        {MODE_IX, MODE_NONE, kTenantOwned, MODE_NONE, MODE_X, false},
        {MODE_IX, MODE_NONE, kTenantOwned, MODE_NONE, MODE_IX, false},
        {MODE_IX, MODE_NONE, kTenantOwned, MODE_NONE, MODE_S, false},
        {MODE_IX, MODE_NONE, kTenantOwned, MODE_NONE, MODE_IS, false},
        {MODE_IX, MODE_X, kTenantOwned, MODE_NONE, MODE_X, true},
        {MODE_IX, MODE_X, kTenantOwned, MODE_NONE, MODE_IX, true},
        {MODE_IX, MODE_X, kTenantOwned, MODE_NONE, MODE_S, true},
        {MODE_IX, MODE_X, kTenantOwned, MODE_NONE, MODE_IS, true},
        {MODE_IS, MODE_NONE, kTenantOwned, MODE_NONE, MODE_X, false},
        {MODE_IS, MODE_NONE, kTenantOwned, MODE_NONE, MODE_IX, false},
        {MODE_IS, MODE_NONE, kTenantOwned, MODE_NONE, MODE_S, false},
        {MODE_IS, MODE_NONE, kTenantOwned, MODE_NONE, MODE_IS, false},
        {MODE_IS, MODE_S, kTenantOwned, MODE_NONE, MODE_X, false},
        {MODE_IS, MODE_S, kTenantOwned, MODE_NONE, MODE_IX, false},
        {MODE_IS, MODE_S, kTenantOwned, MODE_NONE, MODE_S, true},
        {MODE_IS, MODE_S, kTenantOwned, MODE_NONE, MODE_IS, true},
        // Global, tenant, db locks acquired.
        {MODE_NONE, MODE_NONE, kAll, MODE_NONE, MODE_X, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_NONE, MODE_IX, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_NONE, MODE_S, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_NONE, MODE_IS, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_S, MODE_X, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_S, MODE_IX, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_S, MODE_S, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_S, MODE_IS, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_X, MODE_X, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_X, MODE_IX, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_X, MODE_S, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_X, MODE_IS, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_IX, MODE_X, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_IX, MODE_IX, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_IX, MODE_S, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_IX, MODE_IS, true},
        {MODE_NONE, MODE_NONE, kAll, MODE_IS, MODE_X, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_IS, MODE_IX, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_IS, MODE_S, false},
        {MODE_NONE, MODE_NONE, kAll, MODE_IS, MODE_IS, true},
    };
    for (auto&& testCase : testCases) {
        {
            for (auto&& tenantOwned : std::vector<bool>{false, true}) {
                if (!tenantOwned && kTenantOwned == testCase.databaseOwnership) {
                    continue;
                }
                const DatabaseName databaseName = DatabaseName::createDatabaseName_forTest(
                    tenantOwned ? boost::make_optional(tenantId) : boost::none, testDatabaseName);
                boost::optional<Lock::GlobalLock> globalLock;
                boost::optional<Lock::TenantLock> tenantLock;
                boost::optional<Lock::DBLock> dbLock;

                if (MODE_NONE != testCase.globalLockMode) {
                    globalLock.emplace(opCtx.get(), testCase.globalLockMode);
                }
                if (MODE_NONE != testCase.tenantLockMode) {
                    tenantLock.emplace(opCtx.get(), tenantId, testCase.tenantLockMode);
                }
                if (MODE_NONE != testCase.databaseLockMode) {
                    dbLock.emplace(opCtx.get(), databaseName, testCase.databaseLockMode);
                }
                ASSERT(
                    lockState->isDbLockedForMode(databaseName, testCase.checkedDatabaseLockMode) ==
                    testCase.expectedResult)
                    << " global lock mode: " << modeName(testCase.globalLockMode)
                    << " tenant lock mode: " << modeName(testCase.tenantLockMode)
                    << " db lock mode: " << modeName(testCase.databaseLockMode)
                    << " tenant owned: " << tenantOwned
                    << " checked lock mode: " << modeName(testCase.checkedDatabaseLockMode);

                // If database is not locked with intent lock, a collection in the database is
                // locked for the same lock mode.
                ASSERT(testCase.databaseLockMode == MODE_IS ||
                       testCase.databaseLockMode == MODE_IX ||
                       lockState->isCollectionLockedForMode(
                           NamespaceString::createNamespaceString_forTest(databaseName, "coll"),
                           testCase.checkedDatabaseLockMode) == testCase.expectedResult)
                    << " global lock mode: " << modeName(testCase.globalLockMode)
                    << " tenant lock mode: " << modeName(testCase.tenantLockMode)
                    << " db lock mode: " << modeName(testCase.databaseLockMode)
                    << " tenant owned: " << tenantOwned
                    << " checked lock mode: " << modeName(testCase.checkedDatabaseLockMode);
            }
        }
    }
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IS) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db1.coll");

    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

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
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("db1.coll");

    auto opCtx = makeOperationContextWithLocker();
    auto lockState = shard_role_details::getLocker(opCtx.get());

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

    DatabaseName fooDb = DatabaseName::createDatabaseName_forTest(boost::none, "foo");
    DatabaseName localDb = DatabaseName::kLocal;

    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                if (i % 7 == 0 && threadId == 0 /* Only one upgrader legal */) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    ASSERT(shard_role_details::getLocker(clients[threadId].second.get())->isW());
                } else if (i % 7 == 1) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                               ->isReadLocked());
                } else if (i % 7 == 2) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    ASSERT(shard_role_details::getLocker(clients[threadId].second.get())->isW());
                } else if (i % 7 == 3) {
                    Lock::GlobalWrite w(clients[threadId].second.get());

                    Lock::GlobalRead r(clients[threadId].second.get());

                    ASSERT(shard_role_details::getLocker(clients[threadId].second.get())->isW());
                } else if (i % 7 == 4) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    Lock::GlobalRead r2(clients[threadId].second.get());
                    ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                               ->isReadLocked());
                } else if (i % 7 == 5) {
                    { Lock::DBLock r(clients[threadId].second.get(), fooDb, MODE_S); }
                    {
                        Lock::DBLock r(clients[threadId].second.get(),
                                       DatabaseName::createDatabaseName_forTest(boost::none, "bar"),
                                       MODE_S);
                    }
                } else if (i % 7 == 6) {
                    if (i > kNumIterations / 2) {
                        int q = i % 11;

                        if (q == 0) {
                            Lock::DBLock r(clients[threadId].second.get(), fooDb, MODE_S);
                            ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                                       ->isDbLockedForMode(fooDb, MODE_S));

                            Lock::DBLock r2(clients[threadId].second.get(), fooDb, MODE_S);
                            ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                                       ->isDbLockedForMode(fooDb, MODE_S));

                            Lock::DBLock r3(clients[threadId].second.get(), localDb, MODE_S);
                            ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                                       ->isDbLockedForMode(fooDb, MODE_S));
                            ASSERT(shard_role_details::getLocker(clients[threadId].second.get())
                                       ->isDbLockedForMode(localDb, MODE_S));
                        } else if (q == 1) {
                            // test locking local only -- with no preceding lock
                            { Lock::DBLock x(clients[threadId].second.get(), localDb, MODE_S); }

                            Lock::DBLock x(clients[threadId].second.get(), localDb, MODE_X);

                        } else if (q == 2) {
                            {
                                Lock::DBLock x(
                                    clients[threadId].second.get(), DatabaseName::kAdmin, MODE_S);
                            }
                            {
                                Lock::DBLock x(
                                    clients[threadId].second.get(), DatabaseName::kAdmin, MODE_X);
                            }
                        } else if (q == 3) {
                            Lock::DBLock x(clients[threadId].second.get(), fooDb, MODE_X);
                            Lock::DBLock y(
                                clients[threadId].second.get(), DatabaseName::kAdmin, MODE_S);
                        } else if (q == 4) {
                            Lock::DBLock x(
                                clients[threadId].second.get(),
                                DatabaseName::createDatabaseName_forTest(boost::none, "foo2"),
                                MODE_S);
                            Lock::DBLock y(
                                clients[threadId].second.get(), DatabaseName::kAdmin, MODE_S);
                        } else if (q == 5) {
                            Lock::DBLock x(clients[threadId].second.get(), fooDb, MODE_IS);
                        } else if (q == 6) {
                            Lock::DBLock x(clients[threadId].second.get(), fooDb, MODE_IX);
                            Lock::DBLock y(clients[threadId].second.get(), localDb, MODE_IX);
                        } else {
                            Lock::DBLock w(clients[threadId].second.get(), fooDb, MODE_X);

                            Lock::DBLock r2(clients[threadId].second.get(), fooDb, MODE_S);
                            Lock::DBLock r3(clients[threadId].second.get(), localDb, MODE_S);
                        }
                    } else {
                        Lock::DBLock r(clients[threadId].second.get(), fooDb, MODE_S);
                        Lock::DBLock r2(clients[threadId].second.get(), fooDb, MODE_S);
                        Lock::DBLock r3(clients[threadId].second.get(), localDb, MODE_S);
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
                    Lock::DBLock x(clients[threadId].second.get(),
                                   DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                                   MODE_IS);
                } else {
                    Lock::DBLock x(clients[threadId].second.get(),
                                   DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                                   MODE_IX);
                    Lock::DBLock y(clients[threadId].second.get(), DatabaseName::kLocal, MODE_IX);
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
    auto opCtx = makeOperationContext();

    Lock::ResourceMutex mutex("label");
    ASSERT_EQ("label", *ResourceCatalog::get().name(mutex.getRid()));
    Lock::ResourceMutex mutex2("label2");
    ASSERT_EQ("label2", *ResourceCatalog::get().name(mutex2.getRid()));
}

TEST_F(DConcurrencyTestFixture, Throttling) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opctx1 = clientOpctxPairs[0].second.get();
        auto opctx2 = clientOpctxPairs[1].second.get();

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

            // Timeouts should be reasonably immediate. In maxTries attempts at least one test
            // should be able to complete within a second, as the theoretical test duration is less
            // than 50 ms.
            overlongWait = t2 - t1 >= Seconds(1);
        } while (overlongWait && ++tries < maxTries);
        ASSERT(!overlongWait);
    };

    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, NoThrottlingWhenNotAcquiringTickets) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opctx1 = clientOpctxPairs[0].second.get();
        auto opctx2 = clientOpctxPairs[1].second.get();

        // Prevent the enforcement of ticket throttling.
        shard_role_details::getLocker(opctx1)->setAdmissionPriority(
            AdmissionContext::Priority::kImmediate);

        // Both locks should be acquired immediately because there is no throttling.
        Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R1.isLocked());

        Lock::GlobalRead R2(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R2.isLocked());
    };

    // Limit the locker to 1 ticket at a time.
    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, ReleaseAndReacquireTicket) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opctx1 = clientOpctxPairs[0].second.get();
        auto opctx2 = clientOpctxPairs[1].second.get();

        Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R1.isLocked());

        {
            // A second Locker should not be able to acquire a ticket.
            ASSERT_THROWS_CODE(
                Lock::GlobalRead(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow),
                AssertionException,
                ErrorCodes::LockTimeout);
        }

        shard_role_details::getLocker(opctx1)->releaseTicket();

        {
            // Now a second Locker can acquire a ticket.
            Lock::GlobalRead R2(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow);
            ASSERT(R2.isLocked());
        }

        shard_role_details::getLocker(opctx1)->reacquireTicket(opctx1);

        {
            // Now a second Locker cannot acquire a ticket.
            ASSERT_THROWS_CODE(
                Lock::GlobalRead(opctx2, Date_t::now(), Lock::InterruptBehavior::kThrow),
                AssertionException,
                ErrorCodes::LockTimeout);
        }
    };

    // Limit the locker to 1 ticket at a time.
    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, LockerWithReleasedTicketCanBeUnlocked) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();

    Lock::GlobalRead R1(opctx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(R1.isLocked());

    shard_role_details::getLocker(opctx1)->releaseTicket();
}

TEST_F(DConcurrencyTestFixture, TicketAcquireCanThrowDueToKill) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(1);
        auto opctx1 = clientOpctxPairs[0].second.get();

        // This thread should block because it cannot acquire a ticket and then get interrupted.
        auto result = runTaskAndKill(opctx1, [&] { Lock::GlobalRead R2(opctx1); });

        ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    };

    // Limit the locker to 0 tickets at a time.
    int numTickets = 0;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireCanThrowDueToDeadline) {
    auto runTest = [&]() {
        auto clients = makeKClientsWithLockers(1);
        auto opCtx = clients[0].second.get();

        ASSERT_THROWS_CODE(Lock::GlobalLock(opCtx,
                                            MODE_IX,
                                            Date_t::now() + Milliseconds(1500),
                                            Lock::InterruptBehavior::kThrow),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    };

    // Limit the locker to 0 tickets at a time.
    int numTickets = 0;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireShouldNotThrowIfBehaviorIsLeaveUnlocked1) {
    auto runTest = [&]() {
        auto clients = makeKClientsWithLockers(1);
        auto opCtx = clients[0].second.get();

        Lock::GlobalLock(opCtx,
                         MODE_IX,
                         Date_t::now() + Milliseconds(1500),
                         Lock::InterruptBehavior::kLeaveUnlocked);
    };

    int numTickets = 0;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireShouldNotThrowIfBehaviorIsLeaveUnlocked2) {
    auto runTest = [&]() {
        auto clients = makeKClientsWithLockers(1);
        auto opCtx = clients[0].second.get();

        boost::optional<Lock::GlobalLock> globalLock;
        globalLock.emplace(opCtx,
                           MODE_IX,
                           Date_t::now() + Milliseconds(1500),
                           Lock::InterruptBehavior::kLeaveUnlocked);
        ASSERT(!globalLock->isLocked());
    };

    int numTickets = 0;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, TicketAcquireWithMaxDeadlineRespectsUninterruptibleLockGuard) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opCtx1 = clientOpctxPairs[0].second.get();
        auto opCtx2 = clientOpctxPairs[1].second.get();

        // Take the only ticket available.
        boost::optional<Lock::GlobalRead> R1;
        R1.emplace(opCtx1, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R1->isLocked());

        boost::optional<Lock::GlobalRead> R2;

        // Block until a ticket is available.
        auto result = runTaskAndKill(
            opCtx2,
            [&] {
                UninterruptibleLockGuard noInterrupt(  // NOLINT.
                    shard_role_details::getLocker(opCtx2));
                R2.emplace(opCtx2, Date_t::max(), Lock::InterruptBehavior::kThrow);
            },
            [&] {
                // Relase the only ticket available to unblock the other thread.
                R1.reset();
            });

        result.get();  // This should not throw.
        ASSERT(R2->isLocked());
    };

    // Limit the locker to 1 ticket at a time.
    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture, TicketReacquireCanBeInterrupted) {
    auto runTest = [&]() {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opctx1 = clientOpctxPairs[0].second.get();
        auto opctx2 = clientOpctxPairs[1].second.get();

        Lock::GlobalLock R1(
            opctx1, LockMode::MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R1.isLocked());

        {
            // A second Locker should not be able to acquire a ticket.

            ASSERT_THROWS_CODE(
                Lock::GlobalLock(
                    opctx2, LockMode::MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow),
                AssertionException,
                ErrorCodes::LockTimeout);
        }

        shard_role_details::getLocker(opctx1)->releaseTicket();

        // Now a second Locker can acquire a ticket.
        Lock::GlobalLock R2(
            opctx2, LockMode::MODE_IS, Date_t::now(), Lock::InterruptBehavior::kThrow);
        ASSERT(R2.isLocked());

        // This thread should block because it cannot acquire a ticket.
        auto result = runTaskAndKill(
            opctx1, [&] { shard_role_details::getLocker(opctx1)->reacquireTicket(opctx1); });

        ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    };

    // Limit the locker to 1 ticket at a time.
    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
}

TEST_F(DConcurrencyTestFixture,
       TicketReacquireWithTicketExhaustionAndConflictingLockThrowsLockTimeout) {
    auto runTest = [&] {
        auto clientOpctxPairs = makeKClientsWithLockers(2);
        auto opCtx1 = clientOpctxPairs[0].second.get();
        auto opCtx2 = clientOpctxPairs[1].second.get();

        DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");

        boost::optional<Lock::GlobalLock> globalIX = Lock::GlobalLock{opCtx1, LockMode::MODE_IX};
        boost::optional<Lock::DBLock> dbIX = Lock::DBLock{opCtx1, dbName, LockMode::MODE_IX};
        shard_role_details::getLocker(opCtx1)->releaseTicket();

        stdx::packaged_task<void()> task{[opCtx2, &dbName] {
            Lock::GlobalLock globalIX{opCtx2, LockMode::MODE_IX};
            Lock::DBLock dbX{opCtx2, dbName, LockMode::MODE_X};
        }};
        auto result = task.get_future();
        stdx::thread taskThread{std::move(task)};

        ScopeGuard joinGuard{[&taskThread] {
            taskThread.join();
        }};

        // Wait for the database X lock to conflict.
        while (!shard_role_details::getLocker(opCtx2)->hasLockPending()) {
        }

        ASSERT_THROWS_CODE(shard_role_details::getLocker(opCtx1)->reacquireTicket(opCtx1),
                           AssertionException,
                           ErrorCodes::LockTimeout);

        dbIX.reset();
        globalIX.reset();
    };

    // Limit the locker to 1 ticket at a time.
    int numTickets = 1;
    runWithThrottling<PriorityTicketHolder>(numTickets, runTest);
    runWithThrottling<SemaphoreTicketHolder>(numTickets, runTest);
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

    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.
    Lock::GlobalRead globalReadLock(
        opCtx, Date_t::now(), Lock::InterruptBehavior::kThrow);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextThrowsEvenWhenUncontested) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    boost::optional<Lock::DBLock> dbWriteLock;
    ASSERT_THROWS_CODE(
        dbWriteLock.emplace(
            opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX),
        AssertionException,
        ErrorCodes::Interrupted);
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextThrowsEvenWhenAcquiringRecursively) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbWriteLock(
        opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X);

    opCtx->markKilled();

    {
        boost::optional<Lock::DBLock> recursiveDBWriteLock;
        ASSERT_THROWS_CODE(
            recursiveDBWriteLock.emplace(
                opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_X),
            AssertionException,
            ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture, DBLockInInterruptedContextRespectsUninterruptibleGuard) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    opCtx->markKilled();

    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.
    Lock::DBLock dbWriteLock(opCtx,
                             DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                             MODE_X);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, DBLockTimeout) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();

    const Milliseconds timeoutMillis = Milliseconds(1500);

    DatabaseName testDb = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");

    Lock::DBLock L1(opctx1, testDb, MODE_X, Date_t::max());
    ASSERT(shard_role_details::getLocker(opctx1)->isDbLockedForMode(testDb, MODE_X));
    ASSERT(L1.isLocked());

    Date_t t1 = Date_t::now();
    ASSERT_THROWS_CODE(Lock::DBLock(opctx2, testDb, MODE_X, Date_t::now() + timeoutMillis),
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
    ASSERT_THROWS_CODE(Lock::DBLock(opctx2,
                                    DatabaseName::createDatabaseName_forTest(boost::none, "testdb"),
                                    MODE_X,
                                    Date_t::now() + timeoutMillis),
                       AssertionException,
                       ErrorCodes::LockTimeout);
    Date_t t2 = Date_t::now();
    ASSERT_GTE(t2 - t1 + kMaxClockJitterMillis, Milliseconds(timeoutMillis));
}

TEST_F(DConcurrencyTestFixture, CollectionLockInInterruptedContextThrowsEvenWhenUncontested) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(
        opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
    opCtx->markKilled();

    {
        boost::optional<Lock::CollectionLock> collLock;
        ASSERT_THROWS_CODE(
            collLock.emplace(
                opCtx, NamespaceString::createNamespaceString_forTest("db.coll"), MODE_IX),
            AssertionException,
            ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture,
       CollectionLockInInterruptedContextThrowsEvenWhenAcquiringRecursively) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(
        opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);
    Lock::CollectionLock collLock(
        opCtx, NamespaceString::createNamespaceString_forTest("db.coll"), MODE_X);

    opCtx->markKilled();

    {
        boost::optional<Lock::CollectionLock> recursiveCollLock;
        ASSERT_THROWS_CODE(
            recursiveCollLock.emplace(
                opCtx, NamespaceString::createNamespaceString_forTest("db.coll"), MODE_X),
            AssertionException,
            ErrorCodes::Interrupted);
    }
}

TEST_F(DConcurrencyTestFixture, CollectionLockInInterruptedContextRespectsUninterruptibleGuard) {
    auto clients = makeKClientsWithLockers(1);
    auto opCtx = clients[0].second.get();

    Lock::DBLock dbLock(
        opCtx, DatabaseName::createDatabaseName_forTest(boost::none, "db"), MODE_IX);

    opCtx->markKilled();

    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.
    Lock::CollectionLock collLock(opCtx,
                                  NamespaceString::createNamespaceString_forTest("db.coll"),
                                  MODE_IX);  // Does not throw.
}

TEST_F(DConcurrencyTestFixture, CollectionLockTimeout) {
    auto clientOpctxPairs = makeKClientsWithLockers(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();

    const Milliseconds timeoutMillis = Milliseconds(1500);

    DatabaseName testDb = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");

    Lock::DBLock DBL1(opctx1, testDb, MODE_IX, Date_t::max());
    ASSERT(shard_role_details::getLocker(opctx1)->isDbLockedForMode(testDb, MODE_IX));
    Lock::CollectionLock CL1(opctx1,
                             NamespaceString::createNamespaceString_forTest("testdb.test"),
                             MODE_X,
                             Date_t::max());
    ASSERT(shard_role_details::getLocker(opctx1)->isCollectionLockedForMode(
        NamespaceString::createNamespaceString_forTest("testdb.test"), MODE_X));

    Date_t t1 = Date_t::now();
    Lock::DBLock DBL2(opctx2, testDb, MODE_IX, Date_t::max());
    ASSERT(shard_role_details::getLocker(opctx2)->isDbLockedForMode(testDb, MODE_IX));
    ASSERT_THROWS_CODE(
        Lock::CollectionLock(opctx2,
                             NamespaceString::createNamespaceString_forTest("testdb.test"),
                             MODE_X,
                             Date_t::now() + timeoutMillis),
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
    shard_role_details::setRecoveryUnit(opCtx,
                                        std::unique_ptr<RecoveryUnit>(recovUnitOwned.release()),
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
    shard_role_details::setRecoveryUnit(opCtx,
                                        std::unique_ptr<RecoveryUnit>(recovUnitOwned.release()),
                                        WriteUnitOfWork::RecoveryUnitState::kActiveUnitOfWork);
    shard_role_details::getLocker(opCtx)->beginWriteUnitOfWork();

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

    shard_role_details::getLocker(opCtx)->endWriteUnitOfWork();
}

TEST_F(DConcurrencyTestFixture, RSTLLockGuardTimeout) {
    auto clients = makeKClientsWithLockers(2);
    auto firstOpCtx = clients[0].second.get();
    auto secondOpCtx = clients[1].second.get();

    // The first opCtx holds the RSTL.
    repl::ReplicationStateTransitionLockGuard firstRSTL(firstOpCtx, MODE_X);
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_EQ(shard_role_details::getLocker(firstOpCtx)
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
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
    ASSERT_EQ(shard_role_details::getLocker(firstOpCtx)
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
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
    ASSERT_EQ(shard_role_details::getLocker(firstOpCtx)
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);


    // The second opCtx enqueues the lock request but cannot acquire it.
    repl::ReplicationStateTransitionLockGuard secondRSTL(
        secondOpCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    ASSERT_FALSE(secondRSTL.isLocked());

    // The first opCtx unlocks so the second opCtx acquires it.
    firstRSTL.reset();
    ASSERT_EQ(shard_role_details::getLocker(firstOpCtx)
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_NONE);


    secondRSTL.waitForLockUntil(Date_t::now());
    ASSERT_TRUE(secondRSTL.isLocked());
    ASSERT_EQ(shard_role_details::getLocker(secondOpCtx)
                  ->getLockMode(resourceIdReplicationStateTransitionLock),
              MODE_X);
}

TEST_F(DConcurrencyTestFixture, RSTLLockGuardResilientToExceptionThrownBeforeWaitForRSTLComplete) {
    auto clients = makeKClientsWithLockers(2);
    auto firstOpCtx = clients[0].second.get();
    auto secondOpCtx = clients[1].second.get();

    // The first opCtx holds the RSTL.
    repl::ReplicationStateTransitionLockGuard firstRSTL(firstOpCtx, MODE_X);
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_TRUE(shard_role_details::getLocker(firstOpCtx)->isRSTLExclusive());

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
    ASSERT_FALSE(shard_role_details::getLocker(secondOpCtx)->isRSTLLocked());

    // Now, make first opCtx to release the lock to test if we can reacquire the lock again.
    ASSERT_TRUE(shard_role_details::getLocker(firstOpCtx)->isRSTLExclusive());
    firstRSTL.release();
    ASSERT_FALSE(firstRSTL.isLocked());

    // If we haven't cleaned the RSTL lock state from the conflict queue in the lock manager after
    // the destruction of secondRSTL, first opCtx won't be able to reacquire the RSTL in X mode.
    firstRSTL.reacquire();
    ASSERT_TRUE(firstRSTL.isLocked());
    ASSERT_TRUE(shard_role_details::getLocker(firstOpCtx)->isRSTLExclusive());
}

TEST_F(DConcurrencyTestFixture, FailPointInLockDoesNotFailUninterruptibleGlobalNonIntentLocks) {
    auto opCtx = makeOperationContext();

    FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

    Locker locker1(opCtx->getServiceContext());
    Locker locker2(opCtx->getServiceContext());
    Locker locker3(opCtx->getServiceContext());

    {
        locker1.lockGlobal(opCtx.get(), MODE_IX);

        // MODE_S attempt.
        stdx::thread t2([&]() {
            UninterruptibleLockGuard noInterrupt(&locker2);  // NOLINT.
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
            UninterruptibleLockGuard noInterrupt(&locker3);  // NOLINT.
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

    Locker locker1(opCtx->getServiceContext());
    Locker locker2(opCtx->getServiceContext());
    Locker locker3(opCtx->getServiceContext());

    // Granted MODE_X lock, fail incoming MODE_S and MODE_X.
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    locker1.lockGlobal(opCtx.get(), MODE_IX);

    {
        locker1.lock(opCtx.get(), resId, MODE_X);

        // MODE_S attempt.
        stdx::thread t2([&]() {
            UninterruptibleLockGuard noInterrupt(&locker2);  // NOLINT.
            locker2.lockGlobal(opCtx.get(), MODE_IS);
            locker2.lock(opCtx.get(), resId, MODE_S);
        });

        // Wait for the thread to attempt to acquire the collection lock in MODE_S.
        waitForLockerToHaveWaitingResource(&locker2);

        locker1.unlock(resId);
        t2.join();
        locker2.unlock(resId);
        locker2.unlockGlobal();
    }

    {
        locker1.lock(opCtx.get(), resId, MODE_X);

        // MODE_X attempt.
        stdx::thread t3([&]() {
            UninterruptibleLockGuard noInterrupt(&locker3);  // NOLINT.
            locker3.lockGlobal(opCtx.get(), MODE_IX);
            locker3.lock(opCtx.get(), resId, MODE_X);
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

TEST_F(DConcurrencyTestFixture, DifferentTenantsTakeDBLockOnConflictingNamespaceOk) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto db = "db1";
    auto tenant1 = TenantId(OID::gen());
    auto tenant2 = TenantId(OID::gen());

    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(tenant1, db);
    DatabaseName dbName2 = DatabaseName::createDatabaseName_forTest(tenant2, db);

    Lock::DBLock r1(opCtx1, dbName1, MODE_X);
    Lock::DBLock r2(opCtx2, dbName2, MODE_X);

    ASSERT(shard_role_details::getLocker(opCtx1)->isDbLockedForMode(dbName1, MODE_X));
    ASSERT(shard_role_details::getLocker(opCtx2)->isDbLockedForMode(dbName2, MODE_X));
}

TEST_F(DConcurrencyTestFixture, ConflictingTenantDBLockThrows) {
    auto clients = makeKClientsWithLockers(2);
    auto opCtx1 = clients[0].second.get();
    auto opCtx2 = clients[1].second.get();

    auto db = "db1";
    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), db);

    Lock::DBLock r1(opCtx1, dbName1, MODE_X);
    ASSERT(shard_role_details::getLocker(opCtx1)->isDbLockedForMode(dbName1, MODE_X));

    auto result = runTaskAndKill(opCtx2, [&]() { Lock::DBLock r2(opCtx2, dbName1, MODE_S); });

    ASSERT_THROWS_CODE(result.get(), AssertionException, ErrorCodes::Interrupted);
    ASSERT(shard_role_details::getLocker(opCtx1)->isDbLockedForMode(dbName1, MODE_X));
}

}  // namespace
}  // namespace mongo
