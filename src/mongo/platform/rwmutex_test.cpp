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

#include "mongo/platform/rwmutex.h"

#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include <shared_mutex>
#include <vector>

namespace mongo {
namespace {

class WriteRarelyRWMutexTest : public unittest::Test {
public:
    void tearDown() override {
        write_rarely_rwmutex_details::resetGlobalLockRegistry_forTest();
    }

    WriteRarelyRWMutex rwMutex;
};

TEST_F(WriteRarelyRWMutexTest, ReaderAndWriteCleanup) {
    {
        auto readLock = rwMutex.readLock();
    }
    {
        auto writeLock = rwMutex.writeLock();
    }
    auto readLock = rwMutex.readLock();
}

TEST_F(WriteRarelyRWMutexTest, ReadersDoNotBlockOnOtherReaders) {
    const size_t kNumReaders = 8;
    unittest::Barrier barrier(kNumReaders + 1);
    std::vector<unittest::JoinThread> threads;

    for (size_t i = 0; i < kNumReaders; ++i) {
        threads.emplace_back([&] {
            auto readLock = rwMutex.readLock();
            barrier.countDownAndWait();
        });
    }

    // This will block until all readers have acquired the lock, so this test will hang if a reader
    // gets blocked behind another reader.
    barrier.countDownAndWait();
}

TEST_F(WriteRarelyRWMutexTest, WriterWaitsForReaders) {
    Atomic<bool> didWrite(false);
    unittest::Barrier barrier(2);
    unittest::JoinThread writer([&] {
        barrier.countDownAndWait();
        auto writeLock = rwMutex.writeLock();
        didWrite.store(true);
    });

    {
        auto readLock = rwMutex.readLock();
        barrier.countDownAndWait();
        while (!isWriteFlagSet_forTest(rwMutex)) {
            // Await the writer to set its write intent.
        }

        // The sleep makes it likely for a misbehaving writer to perform its write before this
        // thread gets to check the state of `didWrite`.
        sleepFor(Microseconds(5));
        ASSERT_FALSE(didWrite.load());
        ASSERT_TRUE(isWriteFlagSet_forTest(rwMutex));
    }

    writer.join();
    ASSERT_TRUE(didWrite.load());
}

TEST_F(WriteRarelyRWMutexTest, NewReadersWaitForWrite) {
    auto wantsToWrite = makePromiseFuture<void>();
    auto canWrite = makePromiseFuture<void>();
    auto doneReading = makePromiseFuture<void>();

    unittest::JoinThread writer([&] {
        auto writeLock = rwMutex.writeLock();
        wantsToWrite.promise.setWith([] {});
        canWrite.future.get();
    });

    wantsToWrite.future.get();

    unittest::JoinThread reader([&] {
        auto readLock = rwMutex.readLock();
        doneReading.promise.setWith([] {});
    });

    while (!hasWaitersOnWriteFlag_forTest(rwMutex)) {
        // Wait until the reader blocks and awaits the writer to release the lock and notify it.
    }
    ASSERT_FALSE(doneReading.future.isReady());

    canWrite.promise.setWith([] {});
    doneReading.future.get();
    ASSERT_FALSE(hasWaitersOnWriteFlag_forTest(rwMutex));
    ASSERT_FALSE(isWriteFlagSet_forTest(rwMutex));
}

TEST_F(WriteRarelyRWMutexTest, MultiReadersAndSingleWriter) {
    // This mimics a multi-reader single-writer use-case with frequent writes. Readers keep reading
    // until they have observed the first `kTargetFibSize` numbers in the fibonacci sequence. After
    // observing each new value, readers notify the writer via a waitable atomic. Readers verify
    // that what they have observed is a valid fibonacci sequence before returning. The writer
    // produces the first `kTargetFibSize` numbers in the sequence, and waits for all readers to
    // observe each new number before producing another one.
    unittest::ThreadAssertionMonitor monitor;
    const size_t kTargetFibSize = 93;  // Gives the biggest fibonacci number that fits in 64 bits.
    WaitableAtomic<uint32_t> updatesByReaders;
    std::vector<uint64_t> fibs = {0, 1, 1};
    uint64_t value = 1;

    const size_t kNumReaders = 16;
    std::vector<stdx::thread> readers(kNumReaders);
    for (auto& reader : readers) {
        reader = monitor.spawn([&] {
            std::vector<uint64_t> myFibs = {0, 1, 1};
            while (myFibs.size() < kTargetFibSize) {
                auto readLock = rwMutex.readLock();
                if (value != myFibs.back()) {
                    myFibs.push_back(value);
                    if (updatesByReaders.addAndFetch(1) % kNumReaders == 0) {
                        updatesByReaders.notifyAll();
                    }
                }
            }

            // Verify that the reader observed a valid fibonacci sequence.
            ASSERT_EQ(myFibs.size(), kTargetFibSize);
            for (size_t i = 2; i < myFibs.size(); ++i) {
                ASSERT_EQ(fibs[i], fibs[i - 1] + fibs[i - 2]);
            }
        });
    }

    while (fibs.size() < kTargetFibSize) {
        auto target = updatesByReaders.load() + kNumReaders;
        {
            auto writeLock = rwMutex.writeLock();
            value = fibs[fibs.size() - 1] + fibs[fibs.size() - 2];
            fibs.push_back(value);
        }

        auto current = updatesByReaders.load();
        while (current != target) {
            current = updatesByReaders.wait(current);
        }
    }

    monitor.notifyDone();
    for (auto& reader : readers) {
        reader.join();
    }
}

TEST_F(WriteRarelyRWMutexTest, MultiWriter) {
    // This test creates a few writers, each taking turn to increment a shared counter. The idea is
    // that writers should never enter the critical section at the same time.
    const size_t kNumWriters = 5;
    const size_t kTargetValue = 10000;

    size_t counter = 0;
    Atomic<int> activeWriters(0);
    auto writerBody = [&](size_t turn) {
        bool keepRunning = true;
        while (keepRunning) {
            auto writeLock = rwMutex.writeLock();
            ASSERT_EQ(activeWriters.fetchAndAdd(1), 0);
            if (counter == kTargetValue) {
                keepRunning = false;
            } else if (counter % kNumWriters == turn) {
                ++counter;  // It is my turn to update the counter.
            }
            ASSERT_EQ(activeWriters.subtractAndFetch(1), 0);
        }
    };

    unittest::ThreadAssertionMonitor monitor;
    std::vector<stdx::thread> writers;
    for (size_t index = 0; index < kNumWriters; ++index) {
        writers.emplace_back(monitor.spawn([index, writerBody] { writerBody(index); }));
    }

    monitor.notifyDone();
    for (auto& writer : writers) {
        writer.join();
    }

    auto readLock = rwMutex.readLock();
    ASSERT_EQ(counter, kTargetValue);
}

TEST(RWMutex, OneWriterAtAnyTime) {
    RWMutex mutex;
    stdx::unique_lock lk(mutex);
    Atomic<bool> isLockedByMainThread{true};
    ASSERT_FALSE(hasWaiters_forTest(mutex));
    ASSERT_TRUE(isWriteIntentSet_forTest(mutex));

    unittest::ThreadAssertionMonitor monitor;
    auto writer = monitor.spawn([&] {
        std::lock_guard anotherLk(mutex);
        ASSERT_FALSE(isLockedByMainThread.load())
            << "Locked a mutex that is already held by another thread";
    });

    // Best effort to allow `writer` to start and try to exclusively acquire `mutex`.
    sleepFor(Microseconds(5));

    isLockedByMainThread.store(false);
    lk.unlock();
    monitor.notifyDone();
    writer.join();
}

TEST(RWMutex, WriterWaitsForReader) {
    RWMutex mutex;
    std::shared_lock lk(mutex);  // NOLINT
    ASSERT_FALSE(hasWaiters_forTest(mutex));
    ASSERT_FALSE(isWriteIntentSet_forTest(mutex));

    unittest::ThreadAssertionMonitor monitor;
    auto writer = monitor.spawn([&] {
        std::lock_guard lk(mutex);
        ASSERT_EQ(getReadersCount_forTest(mutex), 0);
    });

    while (!hasWaiters_forTest(mutex)) {
        // Wait until the writer notices the reader and proceeds to wait for it to retire.
    }
    ASSERT_TRUE(isWriteIntentSet_forTest(mutex));

    lk.unlock();
    // Let the writer proceed with acquiring the lock.
    monitor.notifyDone();
    writer.join();
}

TEST(RWMutex, NewReaderWaitsForWriter) {
    RWMutex mutex;
    stdx::unique_lock lk(mutex);
    ASSERT_FALSE(hasWaiters_forTest(mutex));

    unittest::ThreadAssertionMonitor monitor;
    auto reader = monitor.spawn([&] {
        std::shared_lock lk(mutex);  // NOLINT
        ASSERT_FALSE(isWriteIntentSet_forTest(mutex));
    });

    while (!hasWaiters_forTest(mutex)) {
        // The reader should start waiting on the mutex shortly, so keep checking.
    }

    lk.unlock();
    // The reader may now acquire the lock and make progress.
    monitor.notifyDone();
    reader.join();
}

DEATH_TEST(RWMutex, TooManyReaders, "invariant") {
    RWMutex mutex;
    addReaders_forTest(mutex, RWMutex::kReadersOverflowMask - 1);
    // The following must hit an invariant since it exceeds the maximum number of readers locks.
    mutex.lock_shared();
}

TEST(RWMutex, MultipleReaders) {
    const auto kNumReaders = 16;
    unittest::Barrier barrier(kNumReaders);

    RWMutex mutex;
    std::vector<unittest::JoinThread> readers;
    for (auto i = 0; i < kNumReaders; ++i) {
        readers.emplace_back([&] {
            std::shared_lock lk(mutex);  // NOLINT
            barrier.countDownAndWait();
        });
    }
}

TEST(RWMutex, MultipleReadersAndWriters) {
    // Starts `kNumWorkers` worker threads and have them loop for a total of `kNumIterations`.
    // Worker threads assign a global order to their local loop, and decide on acquiring a read or a
    // write lock based on this global order: if the order is divisible by one thousand, the thread
    // will acquire a write lock, and otherwise it will acquire a read lock. Each worker ensures
    // that there are no readers or writers when successfully acquiring a read or a write lock,
    // respectively.
    const size_t kNumWorkers = 8;
    const size_t kNumIterations = 5'000'000;

    RWMutex mutex;
    Atomic<size_t> counter{0};
    Atomic<int> readers, writers;

    unittest::Barrier barrier(kNumWorkers);
    std::vector<stdx::thread> workers(kNumWorkers);

    unittest::ThreadAssertionMonitor monitor;
    for (auto& worker : workers) {
        worker = monitor.spawn([&] {
            barrier.countDownAndWait();
            while (true) {
                const auto iteration = counter.fetchAndAdd(1);
                if (iteration >= kNumIterations)
                    return;

                if (iteration % 1'000 == 0) {
                    stdx::lock_guard writeLk(mutex);
                    ASSERT_EQ(readers.loadRelaxed(), 0);
                    writers.fetchAndAddRelaxed(1);
                    ON_BLOCK_EXIT([&] { writers.fetchAndSubtractRelaxed(1); });
                } else {
                    std::shared_lock readLk(mutex);  // NOLINT
                    ASSERT_EQ(writers.loadRelaxed(), 0);
                    readers.fetchAndAddRelaxed(1);
                    ON_BLOCK_EXIT([&] { readers.fetchAndSubtractRelaxed(1); });
                }
            }
        });
    }

    monitor.notifyDone();
    for (auto& worker : workers) {
        worker.join();
    }
}

}  // namespace
}  // namespace mongo
