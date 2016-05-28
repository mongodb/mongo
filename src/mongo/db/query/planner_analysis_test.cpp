/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/planner_analysis.h"

#include <set>

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

TEST(QueryPlannerAnalysis, GetSortPatternBasic) {
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1}")));
    ASSERT_EQUALS(fromjson("{a: -1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1}")));
    ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1}")));
    ASSERT_EQUALS(fromjson("{a: 1, b: -1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: -1}")));
    ASSERT_EQUALS(fromjson("{a: -1, b: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 1}")));
    ASSERT_EQUALS(fromjson("{a: -1, b: -1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: -1}")));
}

TEST(QueryPlannerAnalysis, GetSortPatternOtherElements) {
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 0}")));
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 100}")));
    ASSERT_EQUALS(fromjson("{a: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: Infinity}")));
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: true}")));
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: false}")));
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: []}")));
    ASSERT_EQUALS(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: {}}")));

    ASSERT_EQUALS(fromjson("{a: -1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: -100}")));
    ASSERT_EQUALS(fromjson("{a: -1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: -Infinity}")));

    ASSERT_EQUALS(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{}")));
}

TEST(QueryPlannerAnalysis, GetSortPatternSpecialIndexTypes) {
    ASSERT_EQUALS(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'hashed'}")));
    ASSERT_EQUALS(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'text'}")));
    ASSERT_EQUALS(fromjson("{}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: '2dsphere'}")));
    ASSERT_EQUALS(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: ''}")));
    ASSERT_EQUALS(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'foo'}")));

    ASSERT_EQUALS(fromjson("{a: -1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 'text'}")));
    ASSERT_EQUALS(fromjson("{a: -1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: '2dsphere'}")));
    ASSERT_EQUALS(fromjson("{a: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text'}")));
    ASSERT_EQUALS(fromjson("{a: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere'}")));

    ASSERT_EQUALS(fromjson("{a: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text', c: 1}")));
    ASSERT_EQUALS(fromjson("{a: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere',"
                                                                " c: 1}")));

    ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text'}")));
    ASSERT_EQUALS(fromjson("{a: 1, b: 1}"),
                  QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text',"
                                                                " d: 1}")));
}

// Test the generation of sort orders provided by an index scan done by
// IndexScanNode::computeProperties().
TEST(QueryPlannerAnalysis, IxscanSortOrdersBasic) {
    IndexScanNode ixscan;
    ixscan.indexKeyPattern = fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}");

    // Bounds are {a: [[1,1]], b: [[2,2]], c: [[3,3]], d: [[1,5]], e:[[1,1],[2,2]]},
    // all inclusive.
    OrderedIntervalList oil1("a");
    oil1.intervals.push_back(Interval(fromjson("{'': 1, '': 1}"), true, true));
    ixscan.bounds.fields.push_back(oil1);

    OrderedIntervalList oil2("b");
    oil2.intervals.push_back(Interval(fromjson("{'': 2, '': 2}"), true, true));
    ixscan.bounds.fields.push_back(oil2);

    OrderedIntervalList oil3("c");
    oil3.intervals.push_back(Interval(fromjson("{'': 3, '': 3}"), true, true));
    ixscan.bounds.fields.push_back(oil3);

    OrderedIntervalList oil4("d");
    oil4.intervals.push_back(Interval(fromjson("{'': 1, '': 5}"), true, true));
    ixscan.bounds.fields.push_back(oil4);

    OrderedIntervalList oil5("e");
    oil5.intervals.push_back(Interval(fromjson("{'': 1, '': 1}"), true, true));
    oil5.intervals.push_back(Interval(fromjson("{'': 2, '': 2}"), true, true));
    ixscan.bounds.fields.push_back(oil5);

    // Compute and retrieve the set of sorts.
    ixscan.computeProperties();
    const BSONObjSet& sorts = ixscan.getSort();

    // One possible sort is the index key pattern.
    ASSERT(sorts.find(fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}")) != sorts.end());

    // All prefixes of the key pattern.
    ASSERT(sorts.find(fromjson("{a: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{a: 1, b: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{a: 1, b: 1, c: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{a: 1, b: 1, c: 1, d: 1}")) != sorts.end());

    // Additional sorts considered due to point intervals on 'a', 'b', and 'c'.
    ASSERT(sorts.find(fromjson("{b: 1, c: 1, d: 1, e: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{c: 1, d: 1, e: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{d: 1, e: 1}")) != sorts.end());
    ASSERT(sorts.find(fromjson("{d: 1}")) != sorts.end());

    // There should be 9 total sorts: make sure no other ones snuck their way in.
    ASSERT_EQ(9U, sorts.size());
}

TEST(QueryPlannerAnalysis, GeoSkipValidation) {
    BSONObj unsupportedVersion = fromjson("{'2dsphereIndexVersion': 2}");
    BSONObj supportedVersion = fromjson("{'2dsphereIndexVersion': 3}");

    IndexEntry relevantIndex(fromjson("{'geometry.field': '2dsphere'}"));
    IndexEntry irrelevantIndex(fromjson("{'geometry.field': 1}"));
    IndexEntry differentFieldIndex(fromjson("{'geometry.blah': '2dsphere'}"));
    IndexEntry compoundIndex(fromjson("{'geometry.field': '2dsphere', 'a': -1}"));
    IndexEntry unsupportedIndex(fromjson("{'geometry.field': '2dsphere'}"));

    relevantIndex.infoObj = irrelevantIndex.infoObj = differentFieldIndex.infoObj =
        compoundIndex.infoObj = supportedVersion;
    unsupportedIndex.infoObj = unsupportedVersion;

    QueryPlannerParams params;

    std::unique_ptr<FetchNode> fetchNodePtr = stdx::make_unique<FetchNode>();
    std::unique_ptr<GeoMatchExpression> exprPtr = stdx::make_unique<GeoMatchExpression>();

    GeoMatchExpression* expr = exprPtr.get();
    expr->init("geometry.field", nullptr, BSONObj());

    FetchNode* fetchNode = fetchNodePtr.get();
    // Takes ownership.
    fetchNode->filter = std::move(exprPtr);

    OrNode orNode;
    // Takes ownership.
    orNode.children.push_back(fetchNodePtr.release());

    // We should not skip validation if there are no indices.
    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    // We should not skip validation if there is a non 2dsphere index.
    params.indices.push_back(irrelevantIndex);

    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    // We should not skip validation if the 2dsphere index isn't on relevant field.
    params.indices.push_back(differentFieldIndex);

    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    // We should not skip validation if the 2dsphere index version does not support it.

    params.indices.push_back(unsupportedIndex);

    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), false);

    // We should skip validation if there is a relevant 2dsphere index.
    params.indices.push_back(relevantIndex);

    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), true);

    // Reset the test.
    expr->setCanSkipValidation(false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), true);

    // We should skip validation if there is a relevant 2dsphere compound index.

    // Reset the test.
    params.indices.clear();
    expr->setCanSkipValidation(false);

    params.indices.push_back(compoundIndex);

    QueryPlannerAnalysis::analyzeGeo(params, fetchNode);
    ASSERT_EQ(expr->getCanSkipValidation(), true);

    // Reset the test.
    expr->setCanSkipValidation(false);

    QueryPlannerAnalysis::analyzeGeo(params, &orNode);
    ASSERT_EQ(expr->getCanSkipValidation(), true);
}

}  // namespace
