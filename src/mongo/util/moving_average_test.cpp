/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/moving_average.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace mongo {
namespace {

template <typename Func>
void runForAlphas(Func&& func) {
    const double alphas[] = {0.05, 0.1, 0.2, 0.4, 0.8, 0.9, 0.95};
    for (const double alpha : alphas) {
        MovingAverage avg{alpha};
        func(avg);
    }
}

// Verify that `MovingAverage::get()` returns `boost::none` if `addSample` has
// never been called on the object.
TEST(MovingAverageTest, StartsWithNone) {
    runForAlphas([](auto& avg) { ASSERT_EQ(avg.get(), boost::none) << " alpha=" << avg.alpha(); });
}

// Verify that if `MovingAverage::addSample` has been called on the object only
// once, then `get()` returns the value of that sample.
TEST(MovingAverageTest, FirstSampleIsAverage) {
    const double first = -1.337;
    runForAlphas([=](auto& avg) {
        avg.addSample(first);
        ASSERT_EQ(avg.get(), first) << "alpha=" << avg.alpha();
    });
}

// Verify that adding a sample to an exponential moving average results in a
// new average that is between the previous average and the sample.
// Sample from the sine function, for example.
TEST(MovingAverageTest, AverageMovesTowardsSamples) {
    runForAlphas([](auto& avg) {
        double theta = 0;
        double sample = std::sin(theta);
        double oldAvg = avg.addSample(sample);
        const double delta = 0.1;
        theta += delta;
        do {
            sample = std::sin(theta);
            const double newAvg = avg.addSample(sample);
            const auto [below, above] = std::minmax(oldAvg, sample);

            ASSERT_LTE(below, newAvg)
                << "theta=" << theta << " sin(theta)=" << sample << " alpha=" << avg.alpha();
            ASSERT_GTE(above, newAvg)
                << "theta=" << theta << " sin(theta)=" << sample << " alpha=" << avg.alpha();

            oldAvg = newAvg;
            theta += delta;
        } while (theta < 2 * std::numbers::pi);
    });
}

// Verify that `get()` returns the most recent average.
TEST(MovingAverageTest, GetIsConsistentWithAddSampleAndIsIdempotent) {
    runForAlphas([](auto& avg) {
        // arbitrary history of samples
        const double warmup[] = {9898344, -309409, 2.7e-12, 42};

        double mostRecentAvg;
        for (const double sample : warmup) {
            mostRecentAvg = avg.addSample(sample);
        }

        ASSERT_EQ(avg.get(), mostRecentAvg) << "alpha=" << avg.alpha();
        ASSERT_EQ(avg.get(), mostRecentAvg) << "alpha=" << avg.alpha();
    });
}

// Verify that two or more threads can concurrently call any combination of
// `get()` and `addSample(...)` without upsetting code sanitizers like
// ThreadSanitizer (tsan), AddressSansitizer (asan), and
// UndefinedBehaviorSanitizer (ubsan). This test is only relevant when the test
// driver is built with sanitizers (e.g. `--config=dbg_tsan` or
// `--config=dbg_aubsan`).
TEST(MovingAverageTest, ThreadSafe) {
    // At most as many threads as logical cores, or two threads if we don't
    // know the core count.
    const unsigned maxThreads = std::max(2u, std::thread::hardware_concurrency());

    for (unsigned nThreads = 2; nThreads <= maxThreads; ++nThreads) {
        runForAlphas([=](auto& avg) {
            unittest::Barrier startingLine{nThreads};
            std::vector<unittest::JoinThread> threads;
            for (unsigned id = 0; id < nThreads; ++id) {
                threads.emplace_back([&, id]() {
                    // Wait for the other threads to spawn.
                    startingLine.countDownAndWait();

                    // Bang on `avg` for a while.
                    double scratch = id;
                    for (int i = 0; i < 1'000; ++i) {
                        avg.addSample(scratch);
                        const auto got = avg.get();
                        ASSERT_NE(got, boost::none);
                        scratch = *got;
                    }
                });
            }
        });
    }
}

}  // namespace
}  // namespace mongo
