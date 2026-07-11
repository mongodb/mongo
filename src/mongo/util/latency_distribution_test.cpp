// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/latency_distribution.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(LatencyDistributionTest, WorksWithInterpolation) {
    constexpr auto resolution = Microseconds{100};
    LatencyPercentileDistribution distribution{resolution};

    for (int i = 0; i < 100; i++) {
        distribution.addEntry(resolution);
    }

    ASSERT_EQ(distribution.getPercentile(0.0f), Microseconds{0});
    ASSERT_EQ(distribution.getPercentile(0.75f), Microseconds{75});
}

TEST(LatencyDistributionTest, MergesWorkCorrectly) {
    constexpr auto resolution = Microseconds{100};
    LatencyPercentileDistribution distribution1{resolution};
    LatencyPercentileDistribution distribution2{resolution};

    for (int i = 0; i < 100; i++) {
        distribution1.addEntry(resolution);
        distribution2.addEntry(resolution * 2);
    }

    auto merged = distribution1.mergeWith(distribution2);
    ASSERT_EQ(merged.numEntries(), 200);
    ASSERT_EQ(merged.getMax(), Microseconds{200});
    ASSERT_EQ(merged.getPercentile(0.6f), Microseconds{120});
    ASSERT_EQ(merged.getPercentile(0.5f), Microseconds{100});
}

}  // namespace mongo
