/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
