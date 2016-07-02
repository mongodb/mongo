/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace mongo {
namespace {

TEST_F(QueryPlannerTest, PartialIndexEq) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{a: 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}}");

    runQuery(fromjson("{a: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexNot) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{a: {$ne: 1}}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{a: {$ne: -1}}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{a: {$not: {$lte: 0}}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexElemMatchObj) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{'a.b': {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{'a.b': 1}"), filterExpr.get());

    // The following query could in theory use the partial index, but we don't support it right now.
    runQuery(fromjson("{a: {$elemMatch: {b: 1}}}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{a: {$elemMatch: {b: -1}}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexElemMatchObjContainingOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{'a.b': {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{'a.b': 1}"), filterExpr.get());

    // The following query could in theory use the partial index, but we don't support it right now.
    runQuery(fromjson("{a: {$elemMatch: {$or: [{b: 1}, {b: 2}]}}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexElemMatchObjWithBadSubobjectFilter) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{b: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{'a.b': 1}"), filterExpr.get());

    runQuery(fromjson("{a: {$elemMatch: {$or: [{b: 1}, {b: 2}]}}}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{a: {$elemMatch: {b: {$in: [1, 2]}}}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexAndSingleAssignment) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{a: 1, f: 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}}");

    runQuery(fromjson("{a: 1, f: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexAndMultipleAssignments) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{a: {$gte: 0, $lte: 10}, f: 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[0, 10, true, true]]}}}}}");

    runQuery(fromjson("{a: {$gte: 0, $lte: 10}, f: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{$or: [{a: 1}, {_id: 0}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{a: -1}, {_id: 0}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOrContainingAnd) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{$or: [{a: 1, f: 1}, {_id: 0}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1},"
        "bounds: {a: [[1, 1, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{a: 1, f: -1}, {_id: 0}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOrContainingAndMultipleAssignments) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{$or: [{_id: 0, a: 1}, {a: 2}]}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: "
        "{filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}, "
        "bounds: {a: [[2, 2, true, true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {_id: 0}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}, "
        "bounds: {a: [[2, 2, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{_id: 0, a: -1}, {a: 2}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {a: -1}, node: {ixscan: "
        "{filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}, "
        "bounds: {a: [[2, 2, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{_id: 0, a: -1}, {a: -2}]}"));
    assertNumSolutions(0U);
}


TEST_F(QueryPlannerTest, PartialIndexAndContainingOrContainingAnd) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{x: 1, $or: [{a: 1, f: 1}, {_id: 0}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1},"
        "bounds: {a: [[1, 1, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}]}}}}");

    runQuery(fromjson("{x: 1, $or: [{a: 1, f: -1}, {_id: 0}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexAndContainingOrContainingAndSatisfyingPredOutside) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}, g: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{f: 1, g: 1, $or: [{a: 1, x: 1}, {_id: 0}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1, g: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {x: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1},"
        "bounds: {a: [[1, 1, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[0, 0, true, true]]}}}]}}}}");

    // The following query could in theory use the partial index, but we don't support it right now.
    runQuery(fromjson("{f: 1, x: 1, $or: [{a: 1, g: 1}, {_id: 0}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOrContainingAndContainingOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());
    runQuery(fromjson("{$or: [{x: 1, $or: [{a: 1}, {_id: 0}]}, {_id: 1}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {x: 1}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1, 1, true, true]]}}},"
        "{ixscan: {filter: null, pattern: {_id: 1},"
        "bounds: {_id: [[0, 0, true, true]]}}}]}}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[1, 1, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{x: 1, $or: [{a: -1}, {_id: 2}]}, {_id: 1}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexProvidingSort) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuerySortProj(fromjson("{f: 1}"), fromjson("{a: 1}"), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}}}");

    runQuerySortProj(fromjson("{f: -1}"), fromjson("{a: 1}"), BSONObj());
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOrContainingNot) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    runQuery(fromjson("{$or: [{a: {$ne: 1}}, {_id: 1}]}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{$or: [{a: {$ne: -1}}, {_id: 1}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexMultipleSameAnd) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObjA(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExprA = parseMatchExpression(filterObjA);
    addIndex(fromjson("{a: 1}"), filterExprA.get());
    BSONObj filterObjB(fromjson("{g: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExprB = parseMatchExpression(filterObjB);
    addIndex(fromjson("{b: 1}"), filterExprB.get());

    runQuery(fromjson("{a: 1, b: 1, f: 1, g: 1}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1, f: 1, g: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, f: 1, g: 1}, node: {ixscan: "
        "{filter: null, pattern: {b: 1}, "
        "bounds: {b: [[1, 1, true, true]]}}}}}");

    runQuery(fromjson("{a: 1, b: 1, f: 1, g: -1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1, f: 1, g: -1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}}");

    runQuery(fromjson("{a: 1, b: 1, f: -1, g: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexMultipleSameAndCompoundSharedPrefix) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1, b: 1}"), filterExpr.get());
    addIndex(fromjson("{a: 1, c: 1}"), filterExpr.get());

    runQuery(fromjson("{a: 1, f: 1}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}, "
        "bounds: {a: [[1, 1, true, true]], "
        "b: [['MinKey', 'MaxKey', true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1, c: 1}, "
        "bounds: {a: [[1, 1, true, true]], "
        "c: [['MinKey', 'MaxKey', true, true]]}}}}}");

    runQuery(fromjson("{a: 1, f: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexMultipleSameOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObjA(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExprA = parseMatchExpression(filterObjA);
    addIndex(fromjson("{a: 1}"), filterExprA.get());
    BSONObj filterObjB(fromjson("{g: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExprB = parseMatchExpression(filterObjB);
    addIndex(fromjson("{b: 1}"), filterExprB.get());

    runQuery(fromjson("{$or: [{a: 1, f: 1}, {b: 1, g: 1}]}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}},"
        "{fetch: {filter: {g: 1}, node: {ixscan: "
        "{filter: null, pattern: {b: 1}, "
        "bounds: {b: [[1, 1, true, true]]}}}}}]}}");

    runQuery(fromjson("{$or: [{a: 1, f: 1}, {b: 1, g: -1}]}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{$or: [{a: 1, f: -1}, {b: 1, g: -1}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexAndCompoundIndex) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1, b: 1}"), filterExpr.get());

    runQuery(fromjson("{a: 1, b: 1, f: 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}, "
        "bounds: {a: [[1, 1, true, true]], "
        "b: [[1, 1, true, true]]}}}}}");

    runQuery(fromjson("{a: 1, b: 1, f: -1}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexOrCompoundIndex) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1, b: 1}"), filterExpr.get());

    runQuery(fromjson("{$or: [{a: 1, b: 1, f: 1}, {_id: 1}]}"));
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {f: 1}, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}, "
        "bounds: {a: [[1, 1, true, true]], "
        "b: [[1, 1, true, true]]}}}}},"
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [[1, 1, true, true]]}}}]}}}}");

    runQuery(fromjson("{$or: [{a: 1, b: 1, f: -1}, {_id: 1}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexNor) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{f: {$gt: 0}}"));
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj);
    addIndex(fromjson("{a: 1}"), filterExpr.get());

    // The following query could in theory use the partial index, but we don't support it right now.
    runQuery(fromjson("{$nor: [{a: 1, f: 1}, {_id: 1}]}"));
    assertNumSolutions(0U);

    runQuery(fromjson("{$nor: [{a: 1, f: -1}, {_id: 1}]}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexStringComparisonMatchingCollators) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 'cba'}}"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj, &collator);
    addIndex(fromjson("{a: 1}"), filterExpr.get(), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 'abc'}, collation: {locale: 'reverse'}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: 'abc'}, collation: {locale: 'reverse'}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [['cba', 'cba', true, true]]}}}}}");

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 'zaa'}, collation: {locale: 'reverse'}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, PartialIndexNoStringComparisonNonMatchingCollators) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    BSONObj filterObj(fromjson("{a: {$gt: 0}}"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    std::unique_ptr<MatchExpression> filterExpr = parseMatchExpression(filterObj, &collator);
    addIndex(fromjson("{a: 1}"), filterExpr.get(), &collator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 1}, collation: {locale: 'reverse'}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1}, "
        "bounds: {a: [[1, 1, true, true]]}}}}}");
}

}  // namespace
}  // namespace mongo
