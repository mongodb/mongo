// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_planner_common.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

class QueryPlannerCommonTest : public mongo::unittest::Test {
protected:
    /**
     * Make a minimal IndexEntry from a key pattern.
     */
    IndexEntry buildSimpleIndexEntry(const BSONObj& keyPattern) {
        return IndexBoundsBuilderTest::buildSimpleIndexEntry(keyPattern);
    }
};

// Test the analysis of scan directions.
TEST_F(QueryPlannerCommonTest, ForwardScanDirectionIndexScan) {
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode ixscan(testNss, buildSimpleIndexEntry(fromjson("{a: 1, b: 1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 10), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << 1), 1, false));

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
}

TEST_F(QueryPlannerCommonTest, BackwardScanDirectionIndexScan) {
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode ixscan(testNss, buildSimpleIndexEntry(fromjson("{a: 1, b: 1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 10 << "" << 1), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << 1), -1, false));
    ixscan.direction = -1;

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
}

TEST_F(QueryPlannerCommonTest, ForwardScanDirectionReversedIndexScan) {
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode ixscan(testNss, buildSimpleIndexEntry(fromjson("{a: -1, b: -1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 10 << "" << 1), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << -1), 1, false));

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
}


TEST_F(QueryPlannerCommonTest, BackwardScanDirectionReversedIndexScan) {
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode ixscan(testNss, buildSimpleIndexEntry(fromjson("{a: -1, b: -1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 10), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << -1), -1, false));
    ixscan.direction = -1;

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
}

TEST_F(QueryPlannerCommonTest, ScanDirectionCollScan) {
    CollectionScanNode node;  // Default direction is 1.
    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&node, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&node, -1));
}

TEST_F(QueryPlannerCommonTest, ScanDirectionDistinctScan) {
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    DistinctNode node(testNss, buildSimpleIndexEntry(fromjson("{a: 1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 10), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(a);
    node.computeProperties();

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&node, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&node, -1));
}
