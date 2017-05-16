/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

namespace mongo {

extern bool _supportsDocLocking;

namespace {

const int kMaxPerfThreads = 16;    // max number of threads to use for lock perf
const int kMaxStressThreads = 32;  // max number of threads to use for lock stress
const int kMinPerfMillis = 30;     // min duration for reliable timing

/**
 * Temporarily forces setting of the docLockingSupported global for testing purposes.
 */
class ForceSupportsDocLocking {
public:
    explicit ForceSupportsDocLocking(bool supported) : _oldSupportsDocLocking(_supportsDocLocking) {
        _supportsDocLocking = supported;
    }

    ~ForceSupportsDocLocking() {
        _supportsDocLocking = _oldSupportsDocLocking;
    }

private:
    bool _oldSupportsDocLocking;
};

/**
 * A RAII object that instantiates a TicketHolder that limits number of allowed global lock
 * acquisitions to numTickets. The opCtx must live as long as the UseGlobalThrottling instance.
 */
class UseGlobalThrottling {
public:
    explicit UseGlobalThrottling(OperationContext* opCtx, int numTickets)
        : _opCtx(opCtx), _holder(1) {
        _opCtx->lockState()->setGlobalThrottling(&_holder, &_holder);
    }
    ~UseGlobalThrottling() {
        // Reset the global setting as we're about to destroy the ticket holder.
        _opCtx->lockState()->setGlobalThrottling(nullptr, nullptr);
    }

private:
    OperationContext* _opCtx;
    TicketHolder _holder;
};


class DConcurrencyTestFixture : public unittest::Test {
public:
    DConcurrencyTestFixture() : _client(getGlobalServiceContext()->makeClient("testClient")) {}
    ~DConcurrencyTestFixture() {}

    /**
     * Constructs and returns a new OperationContext.
     */
    ServiceContext::UniqueOperationContext makeOpCtx() const {
        auto opCtx = _client->makeOperationContext();
        opCtx->releaseLockState();
        return opCtx;
    }

    /**
     * Returns a vector of Clients of length 'k', each of which has an OperationContext with its
     * lockState set to a DefaultLockerImpl.
     */
    template <typename LockerType>
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeKClientsWithLockers(int k) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(k);
        for (int i = 0; i < k; ++i) {
            auto client = getGlobalServiceContext()->makeClient(
                str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            opCtx->releaseLockState();
            opCtx->setLockState(stdx::make_unique<LockerType>());
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
        return clients;
    }

    /**
     * Calls fn the given number of iterations, spread out over up to maxThreads threads.
     * The threadNr passed is an integer between 0 and maxThreads exclusive. Logs timing
     * statistics for for all power-of-two thread counts from 1 up to maxThreds.
     */
    void perfTest(stdx::function<void(int threadNr)> fn, int maxThreads) {
        for (int numThreads = 1; numThreads <= maxThreads; numThreads *= 2) {
            std::vector<stdx::thread> threads;

            AtomicInt32 ready{0};
            AtomicInt64 elapsedNanos{0};
            AtomicInt64 timedIters{0};

            for (int threadId = 0; threadId < numThreads; threadId++)
                threads.emplace_back([&, threadId]() {
                    // Busy-wait until everybody is ready
                    ready.fetchAndAdd(1);
                    while (ready.load() < numThreads) {
                    }

                    uint64_t micros = 0;
                    int iters;
                    // Ensure at least 16 iterations are done and at least 25 milliseconds is timed
                    for (iters = 16; iters < (1 << 30) && micros < kMinPerfMillis * 1000;
                         iters *= 2) {
                        // Measure the number of loops
                        Timer t;

                        for (int i = 0; i < iters; i++)
                            fn(threadId);

                        micros = t.micros();
                    }

                    elapsedNanos.fetchAndAdd(micros * 1000);
                    timedIters.fetchAndAdd(iters);
                });

            for (auto& thread : threads)
                thread.join();

            log() << numThreads << " threads took: "
                  << elapsedNanos.load() / static_cast<double>(timedIters.load()) << " ns per call"
                  << (kDebugBuild ? " (DEBUG BUILD!)" : "");
        }
    }

private:
    ServiceContext::UniqueClient _client;
};


TEST_F(DConcurrencyTestFixture, WriteConflictRetryInstantiatesOK) {
    writeConflictRetry(nullptr, "", "", [] {});
}

TEST_F(DConcurrencyTestFixture, ResourceMutex) {
    Lock::ResourceMutex mtx("testMutex");
    DefaultLockerImpl locker1;
    DefaultLockerImpl locker2;
    DefaultLockerImpl locker3;

    struct State {
        void check(int n) {
            ASSERT_EQ(step.load(), n);
        }
        void finish(int n) {
            auto actual = step.fetchAndAdd(1);
            ASSERT_EQ(actual, n);
        }
        void waitFor(stdx::function<bool()> cond) {
            while (!cond())
                sleepmillis(0);
        }
        void waitFor(int n) {
            waitFor([this, n]() { return this->step.load() == n; });
        }
        AtomicInt32 step{0};
    } state;

    stdx::thread t1([&]() {
        // Step 0: Single thread acquires shared lock
        state.waitFor(0);
        Lock::SharedLock lk(&locker1, mtx);
        ASSERT(lk.isLocked());
        state.finish(0);

        // Step 4: Wait for t2 to regain its shared lock
        {
            // Check that TempRelease does not actually unlock anything
            Lock::TempRelease yield(&locker1);

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
        lk.lock(MODE_IS);

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
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::GlobalRead globalRead(opCtx.get());
    ASSERT(opCtx->lockState()->isR());
}

TEST_F(DConcurrencyTestFixture, GlobalWrite) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(opCtx->lockState()->isW());
}

TEST_F(DConcurrencyTestFixture, GlobalWriteAndGlobalRead) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(lockState->isW());

    {
        Lock::GlobalRead globalRead(opCtx.get());
        ASSERT(lockState->isW());
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_Timeout) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalLock globalWrite(clients[0].second.get(), MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);
    ASSERT(!globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_Timeout) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
    Lock::GlobalLock globalWrite(clients[0].second.get(), MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);
    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_NoTimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);

    ASSERT(globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);

    ASSERT(!globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, TempReleaseGlobalWrite) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::GlobalWrite globalWrite(opCtx.get());

    {
        Lock::TempRelease tempRelease(lockState);
        ASSERT(!lockState->isLocked());
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture, TempReleaseRecursive) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::GlobalWrite globalWrite(opCtx.get());
    Lock::DBLock lk(opCtx.get(), "SomeDBName", MODE_X);

    {
        Lock::TempRelease tempRelease(lockState);
        ASSERT(lockState->isW());
        ASSERT(lockState->isDbLockedForMode("SomeDBName", MODE_X));
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture, DBLockTakesS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "db", MODE_S);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "db", MODE_X);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesISForAdminIS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "admin", MODE_IS);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_IS);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesSForAdminS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "admin", MODE_S);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminIX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "admin", MODE_IX);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "admin", MODE_X);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, MultipleWriteDBLocksOnSameThread) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock r1(opCtx.get(), "db1", MODE_X);
    Lock::DBLock r2(opCtx.get(), "db1", MODE_X);

    ASSERT(opCtx->lockState()->isDbLockedForMode("db1", MODE_X));
}

TEST_F(DConcurrencyTestFixture, MultipleConflictingDBLocksOnSameThread) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock r1(opCtx.get(), "db1", MODE_X);
    Lock::DBLock r2(opCtx.get(), "db1", MODE_S);

    ASSERT(lockState->isDbLockedForMode("db1", MODE_X));
    ASSERT(lockState->isDbLockedForMode("db1", MODE_S));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForSMode) {
    const std::string dbName("db");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), dbName, MODE_S);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForXMode) {
    const std::string dbName("db");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), dbName, MODE_X);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IS) {
    const std::string ns("db1.coll");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), "db1", MODE_IS);

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_IS);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));

        // TODO: This is TRUE because Lock::CollectionLock converts IS lock to S
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));

        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_S);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IX) {
    const std::string ns("db1.coll");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), "db1", MODE_IX);

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_IX);

        // TODO: This is TRUE because Lock::CollectionLock converts IX lock to X
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_X);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, Stress) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxStressThreads);

    AtomicInt32 ready{0};
    std::vector<stdx::thread> threads;


    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                const bool sometimes = (std::rand() % 15 == 0);

                if (i % 7 == 0 && threadId == 0 /* Only one upgrader legal */) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    if (i % 7 == 2) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 1) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 2) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    if (sometimes) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 3) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    { Lock::TempRelease t(clients[threadId].second->lockState()); }

                    Lock::GlobalRead r(clients[threadId].second.get());
                    if (sometimes) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 4) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    Lock::GlobalRead r2(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 5) {
                    { Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S); }
                    { Lock::DBLock r(clients[threadId].second.get(), "bar", MODE_S); }
                } else if (i % 7 == 6) {
                    if (i > kNumIterations / 2) {
                        int q = i % 11;

                        if (q == 0) {
                            Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "local", MODE_S));
                        } else if (q == 1) {
                            // test locking local only -- with no preceding lock
                            { Lock::DBLock x(clients[threadId].second.get(), "local", MODE_S); }

                            Lock::DBLock x(clients[threadId].second.get(), "local", MODE_X);

                            if (sometimes) {
                                Lock::TempRelease t(clients[threadId].second.get()->lockState());
                            }
                        } else if (q == 2) {
                            { Lock::DBLock x(clients[threadId].second.get(), "admin", MODE_S); }
                            { Lock::DBLock x(clients[threadId].second.get(), "admin", MODE_X); }
                        } else if (q == 3) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_X);
                            Lock::DBLock y(clients[threadId].second.get(), "admin", MODE_S);
                        } else if (q == 4) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo2", MODE_S);
                            Lock::DBLock y(clients[threadId].second.get(), "admin", MODE_S);
                        } else if (q == 5) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IS);
                        } else if (q == 6) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IX);
                            Lock::DBLock y(clients[threadId].second.get(), "local", MODE_IX);
                        } else {
                            Lock::DBLock w(clients[threadId].second.get(), "foo", MODE_X);

                            { Lock::TempRelease t(clients[threadId].second->lockState()); }

                            Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                            Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                        }
                    } else {
                        Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S);
                        Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                        Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                    }
                }

                progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
    { Lock::GlobalWrite w(newClients[0].second.get()); }
    { Lock::GlobalRead r(newClients[1].second.get()); }
}

TEST_F(DConcurrencyTestFixture, StressPartitioned) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxStressThreads);

    AtomicInt32 ready{0};
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
                    Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IS);
                } else {
                    Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IX);
                    Lock::DBLock y(clients[threadId].second.get(), "local", MODE_IX);
                }

                progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
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
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    UseGlobalThrottling throttle(opctx1, 1);

    bool overlongWait;
    int tries = 0;
    const int maxTries = 15;
    const int timeoutMillis = 42;

    do {
        // Test that throttling will correctly handle timeouts.
        Lock::GlobalRead R1(opctx1, 0);
        ASSERT(R1.isLocked());

        Date_t t1 = Date_t::now();
        {
            Lock::GlobalRead R2(opctx2, timeoutMillis);
            ASSERT(!R2.isLocked());
        }
        Date_t t2 = Date_t::now();

        // Test that the timeout did result in at least the requested wait.
        ASSERT_GTE(t2 - t1, Milliseconds(timeoutMillis));

        // Timeouts should be reasonably immediate. In maxTries attempts at least one test should be
        // able to complete within a second, as the theoretical test duration is less than 50 ms.
        overlongWait = t2 - t1 >= Seconds(1);
    } while (overlongWait && ++tries < maxTries);
    ASSERT(!overlongWait);
}


// These tests exercise single- and multi-threaded performance of uncontended lock acquisition. It
// is neither practical nor useful to run them on debug builds.

TEST_F(DConcurrencyTestFixture, PerformanceStdMutex) {
    stdx::mutex mtx;
    perfTest([&](int threadId) { stdx::unique_lock<stdx::mutex> lk(mtx); }, kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceResourceMutexShared) {
    Lock::ResourceMutex mtx("testMutex");
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    perfTest([&](int threadId) { Lock::SharedLock lk(&locker[threadId], mtx); }, kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceResourceMutexExclusive) {
    Lock::ResourceMutex mtx("testMutex");
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    perfTest([&](int threadId) { Lock::ExclusiveLock lk(&locker[threadId], mtx); },
             kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceCollectionIntentSharedLock) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxPerfThreads);
    ForceSupportsDocLocking supported(true);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(clients[threadId].second.get(), "test", MODE_IS);
            Lock::CollectionLock clk(clients[threadId].second->lockState(), "test.coll", MODE_IS);
        },
        kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceCollectionIntentExclusiveLock) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxPerfThreads);
    ForceSupportsDocLocking supported(true);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(clients[threadId].second.get(), "test", MODE_IX);
            Lock::CollectionLock clk(clients[threadId].second->lockState(), "test.coll", MODE_IX);
        },
        kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceMMAPv1CollectionSharedLock) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxPerfThreads);
    ForceSupportsDocLocking supported(false);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(clients[threadId].second.get(), "test", MODE_IS);
            Lock::CollectionLock clk(clients[threadId].second->lockState(), "test.coll", MODE_S);
        },
        kMaxPerfThreads);
}

TEST_F(DConcurrencyTestFixture, PerformanceMMAPv1CollectionExclusive) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxPerfThreads);
    ForceSupportsDocLocking supported(false);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(clients[threadId].second.get(), "test", MODE_IX);
            Lock::CollectionLock clk(clients[threadId].second->lockState(), "test.coll", MODE_X);
        },
        kMaxPerfThreads);
}

}  // namespace
}  // namespace mongo
