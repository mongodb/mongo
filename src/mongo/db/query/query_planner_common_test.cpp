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

#include "mongo/db/query/query_planner_common.h"

#include "mongo/base/string_data.h"
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
    IndexScanNode ixscan(buildSimpleIndexEntry(fromjson("{a: 1, b: 1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 10), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << 1), 1, false));

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
}

TEST_F(QueryPlannerCommonTest, BackwardScanDirectionIndexScan) {
    IndexScanNode ixscan(buildSimpleIndexEntry(fromjson("{a: 1, b: 1}")));
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
    IndexScanNode ixscan(buildSimpleIndexEntry(fromjson("{a: -1, b: -1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 10 << "" << 1), BoundInclusion::kIncludeBothStartAndEndKeys));
    ixscan.bounds.fields.push_back(a);
    ASSERT_TRUE(ixscan.bounds.isValidFor(BSON("a" << -1), 1, false));

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&ixscan, -1));
}


TEST_F(QueryPlannerCommonTest, BackwardScanDirectionReversedIndexScan) {
    IndexScanNode ixscan(buildSimpleIndexEntry(fromjson("{a: -1, b: -1}")));
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
    DistinctNode node(buildSimpleIndexEntry(fromjson("{a: 1}")));
    OrderedIntervalList a{"a"};
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 10), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(a);
    node.computeProperties();

    ASSERT_TRUE(QueryPlannerCommon::scanDirectionsEqual(&node, 1));
    ASSERT_FALSE(QueryPlannerCommon::scanDirectionsEqual(&node, -1));
}
