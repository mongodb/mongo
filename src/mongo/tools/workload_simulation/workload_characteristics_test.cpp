// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/workload_characteristics.h"

#include "mongo/unittest/unittest.h"

namespace mongo::workload_simulation {
namespace {

TEST(ParabolicWorkloadCharacteristics, SanityCheckThroughput) {
    RWPair throughputAtOptimalConcurrency{10'000, 1'000};
    for (int32_t optimal = 10; optimal <= 990; ++optimal) {
        ParabolicWorkloadCharacteristics characteristics{{optimal, optimal},
                                                         throughputAtOptimalConcurrency};
        for (int32_t current = 2; current < 999; ++current) {
            RWPair concurrency{current, current};
            RWPair oneLess{current - 1, current - 1};

            if (current == optimal) {
                ASSERT_EQ(characteristics.readThroughput(concurrency),
                          throughputAtOptimalConcurrency.read);
                ASSERT_EQ(characteristics.writeThroughput(concurrency),
                          throughputAtOptimalConcurrency.write);
            } else {
                ASSERT_LT(characteristics.readThroughput(concurrency),
                          throughputAtOptimalConcurrency.read);
                ASSERT_LT(characteristics.writeThroughput(concurrency),
                          throughputAtOptimalConcurrency.write);
            }

            if (current <= optimal) {
                ASSERT_LTE(characteristics.readThroughput(oneLess),
                           characteristics.readThroughput(concurrency));
                ASSERT_LTE(characteristics.writeThroughput(oneLess),
                           characteristics.writeThroughput(concurrency));
            } else if (current > optimal) {
                ASSERT_GTE(characteristics.readThroughput(oneLess),
                           characteristics.readThroughput(concurrency));
                ASSERT_GTE(characteristics.writeThroughput(oneLess),
                           characteristics.writeThroughput(concurrency));
            }
        }
    }
}

TEST(ParabolicWorkloadCharacteristics, SanityCheckLatencies) {
    RWPair throughputAtOptimalConcurrency{10'000, 1'000};
    for (int32_t optimal = 10; optimal <= 990; ++optimal) {
        ParabolicWorkloadCharacteristics characteristics{
            {optimal, optimal}, throughputAtOptimalConcurrency, 0.0};
        for (int32_t current = 2; current < 999; ++current) {
            RWPair concurrency{current, current};

            // Latencies should lead roughly to expected throughput
            Nanoseconds latency = characteristics.readLatency(concurrency);
            int32_t expectedThroughput = characteristics.readThroughput(concurrency);
            int32_t observedThroughput =
                static_cast<double>(1'000'000'000) * current / latency.count();
            ASSERT(observedThroughput >= 0.95 * expectedThroughput);
            ASSERT(observedThroughput <= 1.05 * expectedThroughput);
        }
    }
}

}  // namespace
}  // namespace mongo::workload_simulation
