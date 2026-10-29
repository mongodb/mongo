// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/moving_average_metric.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(MovingAverageMetricTest, MetricBuilderMovingAverage) {
    MetricTreeSet trees;
    auto& someAverageMetricThing =
        *MetricBuilder<MovingAverageMetric>{"some.averageMetricThing"}.bind(0.2).setTreeSet(&trees);

    // No mention of "averageMetricThing" at first.
    {
        BSONObjBuilder b;
        appendMergedTrees({&trees[ClusterRole::None]}, b);
        ASSERT_BSONOBJ_EQ(b.obj(), BSON("metrics" << BSON("some" << BSONObj{})));
    }

    // But once we `addSample`, the metric appears (and its first value is the first sample).
    {
        someAverageMetricThing.addSample(-8.3);
        BSONObjBuilder b;
        appendMergedTrees({&trees[ClusterRole::None]}, b);
        ASSERT_BSONOBJ_EQ(b.obj(),
                          BSON("metrics" << BSON("some" << BSON("averageMetricThing" << -8.3))));
    }
}

}  // namespace
}  // namespace mongo
