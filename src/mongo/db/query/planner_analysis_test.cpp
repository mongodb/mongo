/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/planner_analysis.h"

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <s2cellid.h>
#include <set>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

using namespace mongo;

namespace {

/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexDescriptor::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

TEST(QueryPlannerAnalysis, GetSortPatternBasic) {
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1, b: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: -1}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: -1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 1}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: -1, b: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: -1}")));
}

TEST(QueryPlannerAnalysis, GetSortPatternOtherElements) {
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 0}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 100}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: Infinity}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: true}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: false}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: []}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: {}}")));

    ASSERT_BSONOBJ_EQ(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -100}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -Infinity}")));

    ASSERT_BSONOBJ_EQ(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{}")));
}

TEST(QueryPlannerAnalysis, GetSortPatternSpecialIndexTypes) {
    ASSERT_BSONOBJ_EQ(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'hashed'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'text'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: '2dsphere'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: ''}")));
    ASSERT_BSONOBJ_EQ(fromjson("{}"), QueryPlannerAnalysis::getSortPattern(fromjson("{a: 'foo'}")));

    ASSERT_BSONOBJ_EQ(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: 'text'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: -1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: -1, b: '2dsphere'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text'}")));
    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere'}")));

    ASSERT_BSONOBJ_EQ(fromjson("{a: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 'text', c: 1}")));
    ASSERT_BSONOBJ_EQ(
        fromjson("{a: 1}"),
        QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: '2dsphere', c: 1}")));

    ASSERT_BSONOBJ_EQ(fromjson("{a: 1, b: 1}"),
                      QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text'}")));
    ASSERT_BSONOBJ_EQ(
        fromjson("{a: 1, b: 1}"),
        QueryPlannerAnalysis::getSortPattern(fromjson("{a: 1, b: 1, c: 'text', d: 1}")));
}

// Test the generation of sort orders provided by an index scan done by
// IndexScanNode::computeProperties().
TEST(QueryPlannerAnalysis, IxscanSortOrdersBasic) {
    IndexScanNode ixscan(buildSimpleIndexEntry(fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}")));

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
    auto sorts = ixscan.providedSorts();

    // One possible sort is the index key pattern.
    ASSERT(sorts.contains(fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}")));

    // All prefixes of the key pattern.
    ASSERT(sorts.contains(fromjson("{a: 1}")));
    ASSERT(sorts.contains(fromjson("{a: -1, b: 1}")));
    ASSERT(sorts.contains(fromjson("{a: 1, b: -1, c: 1}")));
    ASSERT(sorts.contains(fromjson("{a: 1, b: 1, c: -1, d: 1}")));

    // Additional sorts considered due to point intervals on 'a', 'b', and 'c'.
    ASSERT(sorts.contains(fromjson("{b: -1, d: 1, e: 1}")));
    ASSERT(sorts.contains(fromjson("{d: 1, c: 1, e: 1}")));
    ASSERT(sorts.contains(fromjson("{d: 1, c: -1}")));

    // Sorts that are not considered.
    ASSERT_FALSE(sorts.contains(fromjson("{d: -1, e: -1}")));
    ASSERT_FALSE(sorts.contains(fromjson("{e: 1, a: 1}")));
    ASSERT_FALSE(sorts.contains(fromjson("{d: 1, e: -1}")));
    ASSERT_FALSE(sorts.contains(fromjson("{a: 1, d: 1, e: -1}")));

    // Verify that the 'sorts' object has expected internal fields.
    ASSERT(sorts.getIgnoredFields() == std::set<std::string>({"a", "b", "c"}));
    ASSERT_BSONOBJ_EQ(fromjson("{d: 1, e: 1}"), sorts.getBaseSortPattern());
}

TEST(QueryPlannerAnalysis, GeoSkipValidation) {
    BSONObj unsupportedVersion = fromjson("{'2dsphereIndexVersion': 2}");
    BSONObj supportedVersion = fromjson("{'2dsphereIndexVersion': 3}");

    auto relevantIndex = buildSimpleIndexEntry(fromjson("{'geometry.field': '2dsphere'}"));
    auto irrelevantIndex = buildSimpleIndexEntry(fromjson("{'geometry.field': 1}"));
    auto differentFieldIndex = buildSimpleIndexEntry(fromjson("{'geometry.blah': '2dsphere'}"));
    auto compoundIndex = buildSimpleIndexEntry(fromjson("{'geometry.field': '2dsphere', 'a': -1}"));
    auto unsupportedIndex = buildSimpleIndexEntry(fromjson("{'geometry.field': '2dsphere'}"));

    relevantIndex.infoObj = irrelevantIndex.infoObj = differentFieldIndex.infoObj =
        compoundIndex.infoObj = supportedVersion;
    unsupportedIndex.infoObj = unsupportedVersion;

    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};

    std::unique_ptr<FetchNode> fetchNodePtr = std::make_unique<FetchNode>();
    std::unique_ptr<GeoMatchExpression> exprPtr =
        std::make_unique<GeoMatchExpression>("geometry.field"_sd, nullptr, BSONObj());

    GeoMatchExpression* expr = exprPtr.get();

    FetchNode* fetchNode = fetchNodePtr.get();
    // Takes ownership.
    fetchNode->filter = std::move(exprPtr);

    OrNode orNode;
    // Takes ownership.
    orNode.children.push_back(std::move(fetchNodePtr));

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

TEST_F(QueryPlannerTest, ExprQueryHasImprecisePredicatesRemoved) {
    // Ensure that all of the $_internalExpr predicates which get added when optimizing are later
    // removed for an $expr on a collection scan.

    runQuery(fromjson("{$expr: {$eq: ['$a', 123]}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {filter: {$and: [{$expr: {$eq: ['$a', {$const: 123}]}}]}, dir: 1}}");

    // Does not remove an InternalExpr* within an OR.
    runQuery(fromjson("{$or: [{$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$b', 456]}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {filter: {$or: [{$and: [{$expr: {$eq: ['$a', {$const: 123}]}}]},"
        "                        {$and: [{$expr: {$eq: ['$b', {$const: 456}]}}]}]},"
        "dir: 1}}");

    // Does not remove an InternalExpr* within an OR, even when an adjacent $expr is present.
    runQuery(
        fromjson("{or: [{a: {$_internalExprEq: 123}},"
                 "{$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$b', 456]}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {filter: {or: [{a: {$_internalExprEq: 123}},"
        " {$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$b', 456]}}]}, dir: 1}}");

    // Removes an InternalExpr* within an AND when adjacent to precise $expr predicates.
    runQuery(fromjson("{$and: [{$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$b', 456]}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{cscan: {filter: "
        "{$and: [{$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$b', 456]}}]}, dir:1}}");
}

TEST_F(QueryPlannerTest, ExprQueryHasImprecisePredicatesRemovedMix) {
    // Ensure that the query plan generated does not include redundant $_internalExpr expressions.
    const auto filter =
        "{$or: [{$and: [{$expr: {$eq: ['$a', 123]}}, {$expr: {$eq: ['$a1', 123]}}]},"
        "       {$and: [{$expr: {$eq: ['$b', 123]}}, {$expr: {$eq: ['$b1', 123]}}]},"
        "       {$and: [{$or: [{$and: [{$expr: {$eq: ['$c', 123]}}]},"
        "                      {$and: [{$expr: {$eq: ['$c1', 123]}}]}]},"
        "               {$or: [{$and: [{$expr: {$eq: ['$d', 123]}},"
        "                              {$expr: {$eq: ['$d1', 123]}}]},"
        "                      {$and: [{$expr: {$eq: ['$e1', 123]}},"
        "                              {$expr: {$eq: ['$e2', 123]}}]}]}]}]}";

    runQuery(fromjson(filter));

    assertNumSolutions(1U);

    auto filterWithConstant = std::string(filter);
    boost::replace_all(filterWithConstant, "123", "{$const: 123}");
    std::string soln = str::stream() << "{cscan: {filter:" << filterWithConstant << ", dir: 1}}";
    assertSolutionExists(soln);
}

TEST_F(QueryPlannerTest, ExprOnFetchDoesNotIncludeImpreciseFilter) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a" << 1));

    // The residual predicate on b has to be applied after the fetch. Ensure that there is no
    // additional imprecise predicate on the fetch.
    runQuery(fromjson("{$and: [{a: 1}, {$expr: {$eq: ['$b', 99]}}]}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{$expr: {$eq: ['$b', {$const: 99}]}}]}, node: "
        "     {ixscan: {pattern: {a: 1}, "
        "      bounds: {a: [[1,1,true,true]]}}}}}");
}

}  // namespace
