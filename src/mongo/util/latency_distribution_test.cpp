/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/util/latency_distribution.h"

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
