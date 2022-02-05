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

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
const std::string kIndexName = "indexName";

/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner with
 * a view of the available column indexes.
 */
class QueryPlannerColumnarTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // Treat all queries as SBE compatible for this test.
        QueryPlannerTest::setMarkQueriesSbeCompatible(true);

        // We're interested in testing plans that use a columnar index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void addColumnarIndex() {
        params.columnarIndexes.emplace_back(kIndexName);
    }
};

TEST_F(QueryPlannerColumnarTest, InclusionProjectionUsesColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1, _id: 0}, node: {column_ixscan: {filter: {a: {$gt: 3}},"
        "fields: {a: 1}}}}}");
}

TEST_F(QueryPlannerColumnarTest, InclusionProjectionWithSortUsesColumnarIndexAndBlockingSort) {
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSON("a" << 1), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {proj: {spec: {a: 1, _id: 0}, node: "
        "{column_ixscan: "
        "{fields: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerColumnarTest, ExclusionProjectionDoesNotUseColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 0 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists("{proj: {spec: {a: 0, _id: 0}, node: {cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerColumnarTest, NoProjectionDoesNotUseColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << 1), BSONObj(), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$eq: 1}}}}");
}

TEST_F(QueryPlannerColumnarTest, StandardIndexPreferredOverColumnarIndex) {
    addColumnarIndex();
    addIndex(BSON("a" << 1));

    runQuerySortProj(BSON("a" << 5), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists("{proj: {spec: {a:1, _id: 0}, node: {ixscan: {pattern: {a: 1}}}}}");
}
}  // namespace mongo
