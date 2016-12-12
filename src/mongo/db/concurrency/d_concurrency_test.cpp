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
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"

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

TEST(DConcurrency, ResourceMutexLabels) {
    Lock::ResourceMutex mutex("label");
    ASSERT(mutex.getName() == "label");
    Lock::ResourceMutex mutex2("label2");
    ASSERT(mutex2.getName() == "label2");
}


// These tests exercise single- and multi-threaded performance of uncontended lock acquisition. It
// is neither practical nor useful to run them on debug builds.

TEST(Locker, PerformanceStdMutex) {
    stdx::mutex mtx;
    perfTest([&](int threadId) { stdx::unique_lock<stdx::mutex> lk(mtx); }, kMaxPerfThreads);
}

TEST(Locker, PerformanceResourceMutexShared) {
    Lock::ResourceMutex mtx("testMutex");
    std::array<DefaultLockerImpl, kMaxPerfThreads> locker;
    perfTest([&](int threadId) { Lock::SharedLock lk(&locker[threadId], mtx); }, kMaxPerfThreads);
}

TEST(Locker, PerformanceResourceMutexExclusive) {
    Lock::ResourceMutex mtx("testMutex");
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
