// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/metrics/metric_names.h"

#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {
TEST(GetNameTest, Works) {
    ASSERT_EQ(MetricNames::kTest1.getName(), "test_only.metric1");
}
}  // namespace mongo::otel::metrics
