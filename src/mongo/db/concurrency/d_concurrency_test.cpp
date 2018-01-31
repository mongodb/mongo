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
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/operation_context.h"
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
        : _opCtx(opCtx), _holder(numTickets) {
        _opCtx->lockState()->setGlobalThrottling(&_holder, &_holder);
    }
    ~UseGlobalThrottling() noexcept(false) {
        // Reset the global setting as we're about to destroy the ticket holder.
        _opCtx->lockState()->setGlobalThrottling(nullptr, nullptr);
        ASSERT_EQ(_holder.used(), 0);
    }

private:
    OperationContext* _opCtx;
    TicketHolder _holder;
};

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
        auto client =
            getGlobalServiceContext()->makeClient(str::stream() << "test client for thread " << i);
        auto opCtx = client->makeOperationContext();
        opCtx->releaseLockState();
        opCtx->setLockState(stdx::make_unique<LockerType>());
        clients.emplace_back(std::move(client), std::move(opCtx));
    }
    return clients;
}

/**
 * Returns an operation context that has an MMAPV1 locker attached to it.
 */
ServiceContext::UniqueOperationContext makeMMAPOperationContext() {
    auto opCtx = cc().makeOperationContext();
    opCtx->releaseLockState();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    return opCtx;
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
                for (iters = 16; iters < (1 << 30) && micros < kMinPerfMillis * 1000; iters *= 2) {
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

        log() << numThreads
              << " threads took: " << elapsedNanos.load() / static_cast<double>(timedIters.load())
              << " ns per call" << (kDebugBuild ? " (DEBUG BUILD!)" : "");
    }
}

TEST(DConcurrency, ResourceMutex) {
    Lock::ResourceMutex mtx;
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

TEST(DConcurrency, GlobalRead) {
    MMAPV1LockerImpl ls;
    Lock::GlobalRead globalRead(&ls);
    ASSERT(ls.isR());
}

TEST(DConcurrency, GlobalWrite) {
    MMAPV1LockerImpl ls;
    Lock::GlobalWrite globalWrite(&ls);
    ASSERT(ls.isW());
}

TEST(DConcurrency, GlobalWriteAndGlobalRead) {
    MMAPV1LockerImpl ls;

    Lock::GlobalWrite globalWrite(&ls);
    ASSERT(ls.isW());

    {
        Lock::GlobalRead globalRead(&ls);
        ASSERT(ls.isW());
    }

    ASSERT(ls.isW());
}

TEST(DConcurrency, GlobalLockS_Timeout) {
    MMAPV1LockerImpl ls;
    Lock::GlobalLock globalWrite(&ls, MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    {
        MMAPV1LockerImpl lsTry;
        Lock::GlobalLock globalReadTry(&lsTry, MODE_S, 1);
        ASSERT(!globalReadTry.isLocked());
    }
}

TEST(DConcurrency, GlobalLockX_Timeout) {
    MMAPV1LockerImpl ls;
    Lock::GlobalLock globalWrite(&ls, MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    {
        MMAPV1LockerImpl lsTry;
        Lock::GlobalLock globalWriteTry(&lsTry, MODE_X, 1);
        ASSERT(!globalWriteTry.isLocked());
    }
}

TEST(DConcurrency, GlobalLockXSetsGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());

    {
        Lock::GlobalLock globalWrite(opCtx->lockState(), MODE_X, 0);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, GlobalLockIXSetsGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalWrite(opCtx->lockState(), MODE_IX, 0);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, GlobalLockSDoesNotSetGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx->lockState(), MODE_S, 0);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, GlobalLockISDoesNotSetGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx->lockState(), MODE_IS, 0);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, DBLockXSetsGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());

    { Lock::DBLock dbWrite(opCtx->lockState(), "db", MODE_X); }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, DBLockSDoesNotSetGlobalLockTakenOnOperationContext) {
    Client::initThreadIfNotAlready();
    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());

    { Lock::DBLock dbRead(opCtx->lockState(), "db", MODE_S); }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, GlobalLockXDoesNotSetGlobalLockTakenWhenLockAcquisitionTimesOut) {
    Client::initThreadIfNotAlready();
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(clients[0].second.get()->lockState(), MODE_X, 0);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = makeMMAPOperationContext();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalWrite1(opCtx->lockState(), MODE_X, 1);
        ASSERT_FALSE(globalWrite1.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx.get()).getGlobalExclusiveLockTaken());
}

TEST(DConcurrency, GlobalLockS_NoTimeoutDueToGlobalLockS) {
    MMAPV1LockerImpl ls;
    Lock::GlobalRead globalRead(&ls);

    MMAPV1LockerImpl lsTry;
    Lock::GlobalLock globalReadTry(&lsTry, MODE_S, 1);

    ASSERT(globalReadTry.isLocked());
}

TEST(DConcurrency, GlobalLockX_TimeoutDueToGlobalLockS) {
    MMAPV1LockerImpl ls;
    Lock::GlobalRead globalRead(&ls);

    MMAPV1LockerImpl lsTry;
    Lock::GlobalLock globalWriteTry(&lsTry, MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST(DConcurrency, GlobalLockS_TimeoutDueToGlobalLockX) {
    MMAPV1LockerImpl ls;
    Lock::GlobalWrite globalWrite(&ls);

    MMAPV1LockerImpl lsTry;
    Lock::GlobalLock globalReadTry(&lsTry, MODE_S, 1);

    ASSERT(!globalReadTry.isLocked());
}

TEST(DConcurrency, GlobalLockX_TimeoutDueToGlobalLockX) {
    MMAPV1LockerImpl ls;
    Lock::GlobalWrite globalWrite(&ls);

    MMAPV1LockerImpl lsTry;
    Lock::GlobalLock globalWriteTry(&lsTry, MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST(DConcurrency, TempReleaseGlobalWrite) {
    MMAPV1LockerImpl ls;
    Lock::GlobalWrite globalWrite(&ls);

    {
        Lock::TempRelease tempRelease(&ls);
        ASSERT(!ls.isLocked());
    }

    ASSERT(ls.isW());
}

TEST(DConcurrency, TempReleaseRecursive) {
    MMAPV1LockerImpl ls;
    Lock::GlobalWrite globalWrite(&ls);
    Lock::DBLock lk(&ls, "SomeDBName", MODE_X);

    {
        Lock::TempRelease tempRelease(&ls);
        ASSERT(ls.isW());
        ASSERT(ls.isDbLockedForMode("SomeDBName", MODE_X));
    }

    ASSERT(ls.isW());
}

TEST(DConcurrency, DBLockTakesS) {
    MMAPV1LockerImpl ls;

    Lock::DBLock dbRead(&ls, "db", MODE_S);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(ls.getLockMode(resIdDb) == MODE_S);
}

TEST(DConcurrency, DBLockTakesX) {
    MMAPV1LockerImpl ls;

    Lock::DBLock dbWrite(&ls, "db", MODE_X);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(ls.getLockMode(resIdDb) == MODE_X);
}

TEST(DConcurrency, DBLockTakesISForAdminIS) {
    DefaultLockerImpl ls;

    Lock::DBLock dbRead(&ls, "admin", MODE_IS);

    ASSERT(ls.getLockMode(resourceIdAdminDB) == MODE_IS);
}

TEST(DConcurrency, DBLockTakesSForAdminS) {
    DefaultLockerImpl ls;

    Lock::DBLock dbRead(&ls, "admin", MODE_S);

    ASSERT(ls.getLockMode(resourceIdAdminDB) == MODE_S);
}

TEST(DConcurrency, DBLockTakesXForAdminIX) {
    DefaultLockerImpl ls;

    Lock::DBLock dbWrite(&ls, "admin", MODE_IX);

    ASSERT(ls.getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST(DConcurrency, DBLockTakesXForAdminX) {
    DefaultLockerImpl ls;

    Lock::DBLock dbWrite(&ls, "admin", MODE_X);

    ASSERT(ls.getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST(DConcurrency, MultipleWriteDBLocksOnSameThread) {
    MMAPV1LockerImpl ls;

    Lock::DBLock r1(&ls, "db1", MODE_X);
    Lock::DBLock r2(&ls, "db1", MODE_X);

    ASSERT(ls.isDbLockedForMode("db1", MODE_X));
}

TEST(DConcurrency, MultipleConflictingDBLocksOnSameThread) {
    MMAPV1LockerImpl ls;

    Lock::DBLock r1(&ls, "db1", MODE_X);
    Lock::DBLock r2(&ls, "db1", MODE_S);

    ASSERT(ls.isDbLockedForMode("db1", MODE_X));
    ASSERT(ls.isDbLockedForMode("db1", MODE_S));
}

TEST(DConcurrency, IsDbLockedForSMode) {
    const std::string dbName("db");

    MMAPV1LockerImpl ls;

    Lock::DBLock dbLock(&ls, dbName, MODE_S);

    ASSERT(ls.isDbLockedForMode(dbName, MODE_IS));
    ASSERT(!ls.isDbLockedForMode(dbName, MODE_IX));
    ASSERT(ls.isDbLockedForMode(dbName, MODE_S));
    ASSERT(!ls.isDbLockedForMode(dbName, MODE_X));
}

TEST(DConcurrency, IsDbLockedForXMode) {
    const std::string dbName("db");

    MMAPV1LockerImpl ls;

    Lock::DBLock dbLock(&ls, dbName, MODE_X);

    ASSERT(ls.isDbLockedForMode(dbName, MODE_IS));
    ASSERT(ls.isDbLockedForMode(dbName, MODE_IX));
    ASSERT(ls.isDbLockedForMode(dbName, MODE_S));
    ASSERT(ls.isDbLockedForMode(dbName, MODE_X));
}

TEST(DConcurrency, IsCollectionLocked_DB_Locked_IS) {
    const std::string ns("db1.coll");

    MMAPV1LockerImpl ls;

    Lock::DBLock dbLock(&ls, "db1", MODE_IS);

    {
        Lock::CollectionLock collLock(&ls, ns, MODE_IS);

        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!ls.isCollectionLockedForMode(ns, MODE_IX));

        // TODO: This is TRUE because Lock::CollectionLock converts IS lock to S
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_S));

        ASSERT(!ls.isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(&ls, ns, MODE_S);

        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!ls.isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!ls.isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST(DConcurrency, IsCollectionLocked_DB_Locked_IX) {
    const std::string ns("db1.coll");

    MMAPV1LockerImpl ls;

    Lock::DBLock dbLock(&ls, "db1", MODE_IX);

    {
        Lock::CollectionLock collLock(&ls, ns, MODE_IX);

        // TODO: This is TRUE because Lock::CollectionLock converts IX lock to X
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IS));

        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_S));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(&ls, ns, MODE_X);

        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_S));
        ASSERT(ls.isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST(DConcurrency, Stress) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::array<DefaultLockerImpl, kMaxStressThreads> locker;

    AtomicInt32 ready{0};
    std::vector<stdx::thread> threads;

    for (int threadId = 0; threadId < kMaxStressThreads; threadId++)
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                const bool sometimes = (std::rand() % 15 == 0);

                if (i % 7 == 0 && threadId == 0 /* Only one upgrader legal */) {
                    Lock::GlobalWrite w(&locker[threadId]);
                    if (i % 7 == 2) {
                        Lock::TempRelease t(&locker[threadId]);
                    }

                    ASSERT(locker[threadId].isW());
                } else if (i % 7 == 1) {
                    Lock::GlobalRead r(&locker[threadId]);
                    ASSERT(locker[threadId].isReadLocked());
                } else if (i % 7 == 2) {
                    Lock::GlobalWrite w(&locker[threadId]);
                    if (sometimes) {
                        Lock::TempRelease t(&locker[threadId]);
                    }

                    ASSERT(locker[threadId].isW());
                } else if (i % 7 == 3) {
                    Lock::GlobalWrite w(&locker[threadId]);
                    { Lock::TempRelease t(&locker[threadId]); }

                    Lock::GlobalRead r(&locker[threadId]);
                    if (sometimes) {
                        Lock::TempRelease t(&locker[threadId]);
                    }

                    ASSERT(locker[threadId].isW());
                } else if (i % 7 == 4) {
                    Lock::GlobalRead r(&locker[threadId]);
                    Lock::GlobalRead r2(&locker[threadId]);
                    ASSERT(locker[threadId].isReadLocked());
                } else if (i % 7 == 5) {
                    { Lock::DBLock r(&locker[threadId], "foo", MODE_S); }
                    { Lock::DBLock r(&locker[threadId], "bar", MODE_S); }
                } else if (i % 7 == 6) {
                    if (i > kNumIterations / 2) {
                        int q = i % 11;

                        if (q == 0) {
                            Lock::DBLock r(&locker[threadId], "foo", MODE_S);
                            ASSERT(locker[threadId].isDbLockedForMode("foo", MODE_S));

                            Lock::DBLock r2(&locker[threadId], "foo", MODE_S);
                            ASSERT(locker[threadId].isDbLockedForMode("foo", MODE_S));

                            Lock::DBLock r3(&locker[threadId], "local", MODE_S);
                            ASSERT(locker[threadId].isDbLockedForMode("foo", MODE_S));
                            ASSERT(locker[threadId].isDbLockedForMode("local", MODE_S));
                        } else if (q == 1) {
                            // test locking local only -- with no preceding lock
                            { Lock::DBLock x(&locker[threadId], "local", MODE_S); }

                            Lock::DBLock x(&locker[threadId], "local", MODE_X);

                            if (sometimes) {
                                Lock::TempRelease t(&locker[threadId]);
                            }
                        } else if (q == 2) {
                            { Lock::DBLock x(&locker[threadId], "admin", MODE_S); }
                            { Lock::DBLock x(&locker[threadId], "admin", MODE_X); }
                        } else if (q == 3) {
                            Lock::DBLock x(&locker[threadId], "foo", MODE_X);
                            Lock::DBLock y(&locker[threadId], "admin", MODE_S);
                        } else if (q == 4) {
                            Lock::DBLock x(&locker[threadId], "foo2", MODE_S);
                            Lock::DBLock y(&locker[threadId], "admin", MODE_S);
                        } else if (q == 5) {
                            Lock::DBLock x(&locker[threadId], "foo", MODE_IS);
                        } else if (q == 6) {
                            Lock::DBLock x(&locker[threadId], "foo", MODE_IX);
                            Lock::DBLock y(&locker[threadId], "local", MODE_IX);
                        } else {
                            Lock::DBLock w(&locker[threadId], "foo", MODE_X);

                            { Lock::TempRelease t(&locker[threadId]); }

                            Lock::DBLock r2(&locker[threadId], "foo", MODE_S);
                            Lock::DBLock r3(&locker[threadId], "local", MODE_S);
                        }
                    } else {
                        Lock::DBLock r(&locker[threadId], "foo", MODE_S);
                        Lock::DBLock r2(&locker[threadId], "foo", MODE_S);
                        Lock::DBLock r3(&locker[threadId], "local", MODE_S);
                    }
                }

                progressMeter.hit();
            }
        });

    for (auto& thread : threads)
        thread.join();

    {
        MMAPV1LockerImpl ls;
        Lock::GlobalWrite w(&ls);
    }

    {
        MMAPV1LockerImpl ls;
        Lock::GlobalRead r(&ls);
    }
}

TEST(DConcurrency, StressPartitioned) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::array<DefaultLockerImpl, kMaxStressThreads> locker;

    AtomicInt32 ready{0};
    std::vector<stdx::thread> threads;

    for (int threadId = 0; threadId < kMaxStressThreads; threadId++)
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                if (threadId == 0) {
                    if (i % 100 == 0) {
                        Lock::GlobalWrite w(&locker[threadId]);
                        continue;
                    } else if (i % 100 == 1) {
                        Lock::GlobalRead w(&locker[threadId]);
                        continue;
                    }

                    // Intentional fall through
                }

                if (i % 2 == 0) {
                    Lock::DBLock x(&locker[threadId], "foo", MODE_IS);
                } else {
                    Lock::DBLock x(&locker[threadId], "foo", MODE_IX);
                    Lock::DBLock y(&locker[threadId], "local", MODE_IX);
                }

                progressMeter.hit();
            }
        });

    for (auto& thread : threads)
        thread.join();

    {
        MMAPV1LockerImpl ls;
        Lock::GlobalWrite w(&ls);
    }

    {
        MMAPV1LockerImpl ls;
        Lock::GlobalRead r(&ls);
    }
}

TEST(DConcurrency, Throttling) {
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
        Lock::GlobalRead R1(opctx1->lockState(), 0);
        ASSERT(R1.isLocked());

        Date_t t1 = Date_t::now();
        {
            Lock::GlobalRead R2(opctx2->lockState(), timeoutMillis);
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

TEST(DConcurrency, NoThrottlingWhenNotAcquiringTickets) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opctx1, 1);

    // Prevent the enforcement of ticket throttling.
    opctx1->lockState()->setShouldAcquireTicket(false);

    // Both locks should be acquired immediately because there is no throttling.
    Lock::GlobalRead R1(opctx1->lockState(), 0);
    ASSERT(R1.isLocked());

    Lock::GlobalRead R2(opctx2->lockState(), 0);
    ASSERT(R2.isLocked());
}

TEST(DConcurrency, CompatibleFirstWithSXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(3);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();

    // Build a queue of MODE_S <- MODE_X <- MODE_IS, with MODE_S granted.
    Lock::GlobalRead lockS(opctx1->lockState());
    ASSERT(lockS.isLocked());
    Lock::GlobalLock lockX(opctx2->lockState(), MODE_X, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockX.isLocked());

    // A MODE_IS should be granted due to compatibleFirst policy.
    Lock::GlobalLock lockIS(opctx3->lockState(), MODE_IS, 0);
    ASSERT(lockIS.isLocked());

    lockX.waitForLock(0);
    ASSERT(!lockX.isLocked());
}


TEST(DConcurrency, CompatibleFirstWithXSIXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(4);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_IX <- MODE_IS, with MODE_X granted.
    boost::optional<Lock::GlobalWrite> lockX;
    lockX.emplace(opctx1->lockState());
    ASSERT(lockX->isLocked());
    boost::optional<Lock::GlobalLock> lockS;
    lockS.emplace(opctx2->lockState(), MODE_S, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockS->isLocked());
    Lock::GlobalLock lockIX(
        opctx3->lockState(), MODE_IX, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    Lock::GlobalLock lockIS(
        opctx4->lockState(), MODE_IS, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the MODE_X and ensure that MODE_S will switch policy to compatibleFirst
    lockX.reset();
    lockS->waitForLock(0);
    ASSERT(lockS->isLocked());
    ASSERT(!lockIX.isLocked());
    lockIS.waitForLock(0);
    ASSERT(lockIS.isLocked());

    // Now release the MODE_S and ensure that MODE_IX gets locked.
    lockS.reset();
    lockIX.waitForLock(0);
    ASSERT(lockIX.isLocked());
}

TEST(DConcurrency, CompatibleFirstWithXSXIXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(5);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();
    auto opctx5 = clientOpctxPairs[4].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_X <- MODE_IX <- MODE_IS, with the first MODE_X
    // granted and check that releasing it will result in the MODE_IS being granted.
    boost::optional<Lock::GlobalWrite> lockXgranted;
    lockXgranted.emplace(opctx1->lockState());
    ASSERT(lockXgranted->isLocked());

    boost::optional<Lock::GlobalLock> lockX;
    lockX.emplace(opctx3->lockState(), MODE_X, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockX->isLocked());

    // Now request MODE_S: it will be first in the pending list due to EnqueueAtFront policy.
    boost::optional<Lock::GlobalLock> lockS;
    lockS.emplace(opctx2->lockState(), MODE_S, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockS->isLocked());

    Lock::GlobalLock lockIX(
        opctx4->lockState(), MODE_IX, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    Lock::GlobalLock lockIS(
        opctx5->lockState(), MODE_IS, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the granted MODE_X and ensure that MODE_S will switch policy to compatibleFirst,
    // not locking the MODE_X or MODE_IX, but instead granting the final MODE_IS.
    lockXgranted.reset();
    lockS->waitForLock(0);
    ASSERT(lockS->isLocked());

    lockX->waitForLock(0);
    ASSERT(!lockX->isLocked());
    lockIX.waitForLock(0);
    ASSERT(!lockIX.isLocked());

    lockIS.waitForLock(0);
    ASSERT(lockIS.isLocked());
}

TEST(DConcurrency, CompatibleFirstStress) {
    int numThreads = 8;
    int testMicros = 500000;
    AtomicUInt64 readOnlyInterval{0};
    AtomicBool done{false};
    std::vector<uint64_t> acquisitionCount(numThreads);
    std::vector<uint64_t> timeoutCount(numThreads);
    std::vector<uint64_t> busyWaitCount(numThreads);
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(numThreads);

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
            Lock::GlobalRead readLock(opCtx->lockState(), iters % 2);
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
                        lock.emplace(opCtx->lockState(),
                                     iters % 20 ? MODE_IS : MODE_S,
                                     0,
                                     Lock::GlobalLock::EnqueueOnly());
                        // If thread 0 is holding the MODE_S lock while we tried to acquire a
                        // MODE_IS or MODE_S lock, the CompatibleFirst policy guarantees success.
                        auto newInterval = readOnlyInterval.load();
                        invariant(!interval || interval != newInterval || lock->isLocked());
                        lock->waitForLock(0);
                        break;
                    }
                    case 5:
                        busyWait(threadId, iters % 150);
                        lock.emplace(opCtx->lockState(), MODE_X, iters % 2);
                        busyWait(threadId, iters % 10);
                        break;
                    case 6:
                        lock.emplace(opCtx->lockState(), iters % 25 ? MODE_IX : MODE_S, iters % 2);
                        busyWait(threadId, iters % 100);
                        break;
                    case 7:
                        busyWait(threadId, iters % 100);
                        lock.emplace(opCtx->lockState(), iters % 20 ? MODE_IS : MODE_X, 0);
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
        log() << "thread " << threadId << " stats: " << acquisitionCount[threadId]
              << " acquisitions, " << timeoutCount[threadId] << " timeouts, "
              << busyWaitCount[threadId] / 1000000 << "M busy waits";
    }
}

// These tests exercise single- and multi-threaded performance of uncontended lock acquisition. It
// is neither practical nor useful to run them on debug builds.

TEST(Locker, PerformanceStdMutex) {
    stdx::mutex mtx;
    perfTest([&](int threadId) { stdx::unique_lock<stdx::mutex> lk(mtx); }, kMaxPerfThreads);
}

TEST(Locker, PerformanceResourceMutexShared) {
    Lock::ResourceMutex mtx;
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    perfTest([&](int threadId) { Lock::SharedLock lk(&locker[threadId], mtx); }, kMaxPerfThreads);
}

TEST(Locker, PerformanceResourceMutexExclusive) {
    Lock::ResourceMutex mtx;
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    perfTest([&](int threadId) { Lock::ExclusiveLock lk(&locker[threadId], mtx); },
             kMaxPerfThreads);
}

TEST(Locker, PerformanceCollectionIntentSharedLock) {
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    ForceSupportsDocLocking supported(true);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(&locker[threadId], "test", MODE_IS);
            Lock::CollectionLock clk(&locker[threadId], "test.coll", MODE_IS);
        },
        kMaxPerfThreads);
}

TEST(Locker, PerformanceCollectionIntentExclusiveLock) {
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    ForceSupportsDocLocking supported(true);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(&locker[threadId], "test", MODE_IX);
            Lock::CollectionLock clk(&locker[threadId], "test.coll", MODE_IX);
        },
        kMaxPerfThreads);
}

TEST(Locker, PerformanceMMAPv1CollectionSharedLock) {
    std::array<MMAPV1LockerImpl, kMaxPerfThreads> locker;
    ForceSupportsDocLocking supported(false);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(&locker[threadId], "test", MODE_IS);
            Lock::CollectionLock clk(&locker[threadId], "test.coll", MODE_S);
        },
        kMaxPerfThreads);
}

TEST(Locker, PerformanceMMAPv1CollectionExclusive) {
    std::array<MMAPV1LockerImpl, kMaxPerfThreads> locker;
    ForceSupportsDocLocking supported(false);
    perfTest(
        [&](int threadId) {
            Lock::DBLock dlk(&locker[threadId], "test", MODE_IX);
            Lock::CollectionLock clk(&locker[threadId], "test.coll", MODE_X);
        },
        kMaxPerfThreads);
}

}  // namespace
}  // namespace mongo
