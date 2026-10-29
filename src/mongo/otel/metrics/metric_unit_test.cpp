// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/metrics/metric_unit.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {
TEST(ToStringTest, Works) {
    ASSERT_EQ(toString(MetricUnit::kSeconds), "seconds");
}

DEATH_TEST(ToStringDeathTest, DiesOnBadUnit, "11494600") {
    toString(static_cast<MetricUnit>(-1));
}
}  // namespace mongo::otel::metrics
