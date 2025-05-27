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

#include "mongo/util/concurrency/lock_free_read_list.h"

#include "mongo/platform/waitable_atomic.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include <memory>
#include <set>
#include <vector>

namespace mongo {
namespace {

class SyncListTest : public unittest::Test {
public:
    using ListType = LockFreeReadList<int>;

    void setUp() override {
        _list = std::make_unique<ListType>();
    }

    void tearDown() override {
        _list = {};
    }

    ListType& list() {
        return *_list;
    }

private:
    std::unique_ptr<ListType> _list;
};

TEST_F(SyncListTest, AddThenRemoveEntry) {
    auto it = list().add(1);
    ASSERT_TRUE(!!it);
    list().remove(it);
}

TEST_F(SyncListTest, RemoveAnAlreadyRemovedEntry) {
    auto it = list().add(13);
    list().remove(it);
    list().remove(it);
}

TEST_F(SyncListTest, ReuseRemovedEntry) {
    auto it = list().add(31);
    list().remove(it);
    ASSERT_EQ(it, list().add(52));
}

TEST_F(SyncListTest, NonBlockingReads) {
    const auto kValue = 12345;
    auto it = list().add(kValue);

    const auto kThreadCount = 16;
    unittest::ThreadAssertionMonitor monitor;
    std::vector<unittest::JoinThread> threads;
    unittest::Barrier barrier(kThreadCount + 1);
    for (size_t i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(monitor.spawn([&] {
            auto cursor = list().getCursor();
            ASSERT_EQ(cursor.value(), kValue);
            barrier.countDownAndWait();
        }));
    }

    barrier.countDownAndWait();
    monitor.notifyDone();
    list().remove(it);
}

TEST_F(SyncListTest, ReadersDoNotBlockAdd) {
    const auto kV1 = 1, kV2 = 2;
    list().add(kV1);
    auto cursor = list().getCursor();  // Acquires a read-lock on `kV1`.
    ASSERT_EQ(cursor.value(), kV1);
    list().add(kV2);
}

TEST_F(SyncListTest, RemovingAnEntryAwaitsReaders) {
    unittest::Barrier barrier(2);

    const auto kValue = 2024;
    auto it = list().add(kValue);

    auto removed = makePromiseFuture<void>();
    unittest::JoinThread remover([&] {
        barrier.countDownAndWait();
        list().remove(it);
        std::move(removed.promise).setWith([] {});
    });

    {
        auto cursor = list().getCursor();  // Acquires a read-lock on `kValue`.
        ASSERT_EQ(cursor.value(), kValue);
        barrier.countDownAndWait();

        while (auto c = list().getCursor()) {
            // Keep looping until new readers cannot acquire a read-lock (i.e. the writer has set
            // their write intent on the only entry in the list).
        }
        ASSERT_FALSE(removed.future.isReady());
    }

    removed.future.get();
}

TEST_F(SyncListTest, ReadersOfAnEntryDoNotBlockRemovingOthers) {
    const auto kV1 = 5, kV2 = 7;
    auto it1 = list().add(kV1);
    auto it2 = list().add(kV2);
    {
        auto cursor = list().getCursor();  // Acquires a read-lock on the most recent value.
        ASSERT_EQ(cursor.value(), kV2);
        list().remove(it1);
    }
    list().remove(it2);  // We should be able to remove this now that the read-lock is released.
}

TEST_F(SyncListTest, ReadersSkipRemovedEntries) {
    std::vector<int> values = {1, 3, 5, 7, 9, 11, 13, 15};
    std::vector<ListType::Entry*> entries;
    for (auto v : values) {
        entries.push_back(list().add(v));
    }

    // Remove values at odd indexes. Removing after all elements are added ensures entries are not
    // reused (only logically deleted).
    std::set<int> removed;
    for (size_t i = 1; i < values.size(); ++i) {
        list().remove(entries[i]);
        removed.insert(values[i]);
    }

    size_t found = 0;
    for (auto cursor = list().getCursor(); cursor; cursor.next()) {
        ASSERT_FALSE(removed.contains(cursor.value())) << "Found removed element!";
        ++found;
    }
    ASSERT_EQ(found, values.size() - removed.size());
}

TEST_F(SyncListTest, TooManyReaders) {
    auto it = list().add(123);  // Will be locked by all readers.

    // Each entry can support up to 2 ^ 30 readers. We manually set the number of readers so that
    // there is room for only one additional reader.
    list().setReaders_forTest(it, 0x3FFFFFFE);

    auto c1 = list().getCursor();
    ASSERT_EQ(list().getReaders_forTest(it), 0x3FFFFFFF);
    ASSERT_THROWS_CODE(list().getCursor(), DBException, ErrorCodes::TooManyLocks);

    // Make sure the numbers of readers is set zero after `c1` is destroyed.
    list().setReaders_forTest(it, 1);
}

TEST_F(SyncListTest, ReadersObserveTheSameList) {
    // Make a list containing [0, 1023].
    const int kValuesCount = 1024;
    for (int i = kValuesCount - 1; i >= 0; --i) {
        list().add(i);
    }

    const auto kThreadCount = 16;
    unittest::ThreadAssertionMonitor monitor;
    std::vector<unittest::JoinThread> threads;
    for (size_t i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(monitor.spawn([&] {
            int expectedValue = 0;
            for (auto cursor = list().getCursor(); cursor; cursor.next(), ++expectedValue) {
                ASSERT_EQ(cursor.value(), expectedValue);
            }
            ASSERT_EQ(expectedValue, kValuesCount);
        }));
    }

    monitor.notifyDone();
}

TEST_F(SyncListTest, ConcurrentAddAndRemove) {
    Atomic<int> counter;
    const auto kThreadCount = 16;
    unittest::ThreadAssertionMonitor monitor;
    std::vector<unittest::JoinThread> threads;
    for (size_t i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(monitor.spawn([&] {
            auto iterations = 1024;
            while (iterations--) {
                const auto value = counter.fetchAndAdd(1);
                auto it = list().add(value);
                size_t found = 0;
                for (auto cursor = list().getCursor(); cursor; cursor.next()) {
                    if (cursor.value() == value) {
                        ++found;
                    }
                }
                ASSERT_EQ(found, 1) << "Expected to only observe the item once!";
                list().remove(it);
            }
        }));
    }

    monitor.notifyDone();
}

}  // namespace
}  // namespace mongo
