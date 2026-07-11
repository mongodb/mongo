// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/registry_list.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(RegistryList, MixedOperationsSingleThread) {
    /**
     * Show that iterleaved add() and iter() calls function as expected
     */

    RegistryList<boost::optional<int>> list;

    for (int i = 0; i < 100; ++i) {
        // Get our cached iterator first
        auto iter = list.iter();

        {
            // Add a new value
            auto index = list.add(i);

            // Each index should be i since we are adding in sequence
            ASSERT_EQ(index, i);
        }

        {
            // Each index from before should be accessible
            int j = 0;
            for (; j <= i; ++j) {
                // For each value that was already added, the value should also be the index
                ASSERT_EQ(*list.at(j), j);
            }

            // Values outside the list should be default constructed
            ASSERT_FALSE(list.at(j));
        }

        {
            // The iterator we got out should have the state before we added the newest element
            int count = 0;
            while (iter.more()) {
                auto val = iter.next();
                ASSERT_EQ(*val, count);

                ++count;
            }

            ASSERT_EQ(count, list.size() - 1);
            ASSERT_FALSE(iter.next());
        }
    }
}

TEST(RegistryList, ConcurrentAdd) {
    /**
     * Show that multiple concurrent add() and iter() calls function to expectation
     */
    RegistryList<boost::optional<size_t>> list;

    constexpr size_t kThreads = 4;
    constexpr size_t kAdds = 100000;


    struct State {
        std::mutex m;
        size_t workersDone = 0;
    } state;

    unittest::Barrier barrier{kThreads + 1};

    auto addWorkerFunc = [&] {
        // Each worker adds a fixed number of elements
        barrier.countDownAndWait();

        for (size_t i = 0; i < kAdds; ++i) {
            list.add(i);
        }

        std::lock_guard lk(state.m);
        ++state.workersDone;
    };

    auto iteratorFunc = [&] {
        // The iterator thread repeatedly tries to iterate over the list while add()s are happening.
        barrier.countDownAndWait();

        while ([&] {
            std::lock_guard lk(state.m);
            return state.workersDone != kThreads;
        }()) {
            // The list never shrinks over the course of iteration and probably grows, this proves
            // that rule via iter() and size()

            auto lastSize = list.size();
            auto iter = list.iter();

            auto count = 0;
            while (auto e = iter.next()) {
                ++count;
            }

            ASSERT_LTE(lastSize, count);
            ASSERT_LTE(count, list.size());
        }
    };

    std::vector<stdx::thread> threads;
    threads.emplace_back(iteratorFunc);
    for (size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back(addWorkerFunc);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(list.size(), kThreads * kAdds);
}

}  // namespace
}  // namespace mongo
