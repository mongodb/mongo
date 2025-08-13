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

#include <memory>
#include <set>

// IWYU pragma: no_include "boost/align/detail/aligned_alloc_posix.hpp"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/util/partitioned.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const std::size_t nPartitions = 3;
using PartitionedIntSet = Partitioned<std::set<std::size_t>>;
using PartitionedMap = Partitioned<stdx::unordered_map<std::size_t, char>>;

auto makePartitionedIntSet() {
    return PartitionedIntSet(nPartitions);
}
auto makePartitionedMap() {
    return PartitionedMap(nPartitions, {{0, 'a'}, {1, 'b'}, {2, 'c'}});
}

TEST(Partitioned, PartitionedUnorderedMap) {
    auto test = makePartitionedMap();
    ASSERT_EQ(test.count(0), 1UL);
    ASSERT_EQ(test.count(1), 1UL);
    ASSERT_EQ(test.count(2), 1UL);
    ASSERT_EQ(test.count(3), 0UL);
}

TEST(Partitioned, DefaultConstructedPartitionedShouldBeEmpty) {
    auto test = makePartitionedIntSet();
    ASSERT_TRUE(test.empty());
}

TEST(Partitioned, InsertionShouldModifySize) {
    auto test = makePartitionedIntSet();
    test.insert(4);
    ASSERT_EQ(test.size(), 1UL);
    ASSERT_EQ(test.count(4), 1UL);
    ASSERT_FALSE(test.empty());
}

TEST(Partitioned, DuplicateInsertionShouldNotModifySize) {
    auto test = makePartitionedIntSet();
    test.insert(4);
    test.insert(4);
    ASSERT_EQ(test.size(), 1UL);
    ASSERT_EQ(test.count(4), 1UL);
}

TEST(Partitioned, ClearShouldResetSizeToZero) {
    auto test = makePartitionedIntSet();
    test.insert(0);
    test.insert(1);
    test.insert(2);
    ASSERT_EQ(test.size(), 3UL);
    ASSERT_EQ(test.count(0), 1UL);
    ASSERT_EQ(test.count(1), 1UL);
    ASSERT_EQ(test.count(2), 1UL);
    test.clear();
    ASSERT_EQ(test.size(), 0UL);
    ASSERT_TRUE(test.empty());
}

TEST(Partitioned, ErasingEntryShouldModifySize) {
    auto test = makePartitionedIntSet();
    test.insert(0);
    test.insert(1);
    test.insert(2);
    ASSERT_EQ(test.size(), 3UL);
    ASSERT_EQ(test.count(0), 1UL);
    ASSERT_EQ(1UL, test.erase(0));
    ASSERT_EQ(test.count(0), 0UL);
    ASSERT_EQ(test.size(), 2UL);
}

TEST(Partitioned, ErasingEntryThatDoesNotExistShouldNotModifySize) {
    auto test = makePartitionedIntSet();
    test.insert(0);
    test.insert(1);
    test.insert(2);
    ASSERT_EQ(test.size(), 3UL);
    ASSERT_EQ(test.count(5), 0UL);
    ASSERT_EQ(0UL, test.erase(5));
    ASSERT_EQ(test.count(5), 0UL);
    ASSERT_EQ(test.size(), 3UL);
}

TEST(PartitionedAll, DefaultConstructedPartitionedShouldBeEmpty) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    ASSERT_TRUE(all.empty());
}

TEST(PartitionedAll, InsertionShouldModifySize) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    all.insert(4);
    ASSERT_EQ(all.size(), 1UL);
    ASSERT_EQ(all.count(4), 1UL);
    ASSERT_FALSE(all.empty());
}

TEST(PartitionedAll, DuplicateInsertionShouldNotModifySize) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    all.insert(4);
    all.insert(4);
    ASSERT_EQ(all.size(), 1UL);
    ASSERT_EQ(all.count(4), 1UL);
}

TEST(PartitionedAll, ClearShouldResetSizeToZero) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    all.insert(0);
    all.insert(1);
    all.insert(2);
    ASSERT_EQ(all.size(), 3UL);
    ASSERT_EQ(all.count(0), 1UL);
    ASSERT_EQ(all.count(1), 1UL);
    ASSERT_EQ(all.count(2), 1UL);
    all.clear();
    ASSERT_EQ(all.size(), 0UL);
    ASSERT_TRUE(all.empty());
}

TEST(PartitionedAll, ErasingEntryShouldModifySize) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    all.insert(0);
    all.insert(1);
    all.insert(2);
    ASSERT_EQ(all.size(), 3UL);
    ASSERT_EQ(all.count(0), 1UL);
    ASSERT_EQ(1UL, all.erase(0));
    ASSERT_EQ(all.count(0), 0UL);
    ASSERT_EQ(all.size(), 2UL);
}

TEST(PartitionedAll, ErasingEntryThatDoesNotExistShouldNotModifySize) {
    auto test = makePartitionedIntSet();
    auto all = test.lockAllPartitions();
    all.insert(0);
    all.insert(1);
    all.insert(2);
    ASSERT_EQ(all.size(), 3UL);
    ASSERT_EQ(all.count(5), 0UL);
    ASSERT_EQ(0UL, all.erase(5));
    ASSERT_EQ(all.count(5), 0UL);
    ASSERT_EQ(all.size(), 3UL);
}

TEST(PartitionedConcurrency, ShouldBeAbleToGuardSeparatePartitionsSimultaneously) {
    auto test = makePartitionedIntSet();
    {
        auto zeroth = test.lockOnePartition(0);
        auto first = test.lockOnePartition(1);
    }
}

TEST(PartitionedConcurrency, ModificationsFromOnePartitionShouldBeVisible) {
    auto test = makePartitionedIntSet();
    {
        auto zeroth = test.lockOnePartition(0);
        zeroth->insert(0);
    }

    // Make sure a All can see the modifications.
    {
        auto all = test.lockAllPartitions();
        ASSERT_EQ(1UL, all.size());
    }

    // Make sure a OnePartition can see the modifications.
    {
        auto guardedPartition = test.lockOnePartition(0);
        ASSERT_EQ(1UL, guardedPartition->count(0));
    }
}

TEST(PartitionedConcurrency, ModificationsFromAllShouldBeVisible) {
    auto test = makePartitionedIntSet();
    {
        auto all = test.lockAllPartitions();
        all.insert(0);
        all.insert(1);
        all.insert(2);
        for (auto&& partition : all) {
            ASSERT_EQ(1UL, partition->size());
        }
    }

    // Make sure a OnePartition can see the modifications.
    {
        auto zeroth = test.lockOnePartition(0);
        ASSERT_EQ(1UL, zeroth->count(0));
    }
    {
        auto first = test.lockOnePartition(1);
        ASSERT_EQ(1UL, first->count(1));
    }
    {
        auto second = test.lockOnePartition(2);
        ASSERT_EQ(1UL, second->count(2));
    }

    // Make sure a All can see the modifications.
    {
        auto all = test.lockAllPartitions();
        ASSERT_EQ(3UL, all.size());
    }
}

TEST(PartitionedConcurrency, ShouldProtectConcurrentAccesses) {
    auto test = makePartitionedIntSet();

    // 4 threads will be accessing each partition.
    const size_t numThreads = nPartitions * 4;
    std::vector<stdx::thread> threads;
    const size_t opsPerThread = 1000;

    AtomicWord<unsigned> ready{0};
    for (size_t threadId = 1; threadId <= numThreads; ++threadId) {
        auto workerThreadBody = [&, threadId, opsPerThread]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < numThreads) {
            }

            for (size_t op = 0; op < opsPerThread; ++op) {
                size_t partitionId = threadId % nPartitions;
                size_t uniqueVal = (nPartitions * opsPerThread * threadId + op) + partitionId;
                if (op % 3 == 0) {
                    auto all = test.lockAllPartitions();
                    all.insert(uniqueVal);
                } else if (op % 3 == 1) {
                    auto partition = test.lockOnePartition(partitionId);
                    partition->insert(uniqueVal);
                } else if (op % 3 == 2) {
                    test.insert(uniqueVal);
                }
            }
        };

        threads.emplace_back(workerThreadBody);
    }
    for (auto& thread : threads)
        thread.join();

    // Make sure each insert was successful.
    for (std::size_t partitionId = 0; partitionId < nPartitions; ++partitionId) {
        auto partition = test.lockOnePartition(std::size_t{partitionId});
        ASSERT_EQ(opsPerThread * 4, partition->size());
    }
    ASSERT_EQ(numThreads * opsPerThread, test.size());
}
}  // namespace
}  // namespace mongo
