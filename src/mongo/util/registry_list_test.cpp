/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/registry_list.h"

#include <vector>

#include <boost/optional.hpp>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(RegistryList, MixedOperationsSingleThread) {
    /**
     * Show that iterleaved add() and iter() calls function as expected
     */

    RegistryList<boost::optional<int>> list;

    for (int i = 0; i < 10000; ++i) {
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
        stdx::mutex m;  // NOLINT
        size_t workersDone = 0;
    } state;

    unittest::Barrier barrier{kThreads + 1};

    auto addWorkerFunc = [&] {
        // Each worker adds a fixed number of elements
        barrier.countDownAndWait();

        for (size_t i = 0; i < kAdds; ++i) {
            list.add(i);
        }

        stdx::lock_guard lk(state.m);
        ++state.workersDone;
    };

    auto iteratorFunc = [&] {
        // The iterator thread repeatedly tries to iterate over the list while add()s are happening.
        barrier.countDownAndWait();

        while ([&] {
            stdx::lock_guard lk(state.m);
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
