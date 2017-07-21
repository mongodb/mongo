/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <map>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {

using namespace mongo;

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanForCountWhenIndexSatisfiesQuery) {
    params.options = QueryPlannerParams::IS_COUNT;
    addIndex(BSON("x" << 1));
    runQuery(BSON("x" << 5));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{ixscan: {pattern: {x: 1}, bounds: {x: [[5,5,true,true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchToIxscanForCountWhenFetchFilterNonempty) {
    params.options = QueryPlannerParams::IS_COUNT;
    addIndex(BSON("x" << 1));
    runQuery(BSON("y" << 3 << "x" << 5));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {y: 3}, node: {ixscan: "
        "{pattern: {x: 1}, bounds: {x: [[5,5,true,true]]}}}}}");
}

//
// Equality
//

TEST_F(QueryPlannerTest, EqualityIndexScan) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << 5));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, EqualityIndexScanWithTrailingFields) {
    addIndex(BSON("x" << 1 << "y" << 1));

    runQuery(BSON("x" << 5));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
}

// $eq can use a hashed index because it looks for values of type regex;
// it doesn't evaluate the regex itself.
TEST_F(QueryPlannerTest, EqCanUseHashedIndexWithRegex) {
    addIndex(BSON("a"
                  << "hashed"));
    runQuery(fromjson("{a: {$eq: /abc/}}"));
    ASSERT_EQUALS(getNumSolutions(), 2U);
}

//
// indexFilterApplied
// Check that index filter flag is passed from planner params
// to generated query solution.
//

TEST_F(QueryPlannerTest, IndexFilterAppliedDefault) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << 5));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");

    // Check indexFilterApplied in query solutions;
    for (auto it = solns.begin(); it != solns.end(); ++it) {
        QuerySolution* soln = it->get();
        ASSERT_FALSE(soln->indexFilterApplied);
    }
}

TEST_F(QueryPlannerTest, IndexFilterAppliedTrue) {
    params.indexFiltersApplied = true;

    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << 5));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
    assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");

    // Check indexFilterApplied in query solutions;
    for (auto it = solns.begin(); it != solns.end(); ++it) {
        QuerySolution* soln = it->get();
        ASSERT_EQUALS(params.indexFiltersApplied, soln->indexFilterApplied);
    }
}

//
// <
//

TEST_F(QueryPlannerTest, LessThan) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << BSON("$lt" << 5)));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: {$lt: 5}}}}");
    assertSolutionExists("{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
}

//
// <=
//

TEST_F(QueryPlannerTest, LessThanEqual) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << BSON("$lte" << 5)));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: {$lte: 5}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

//
// >
//

TEST_F(QueryPlannerTest, GreaterThan) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << BSON("$gt" << 5)));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: {$gt: 5}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

//
// >=
//

TEST_F(QueryPlannerTest, GreaterThanEqual) {
    addIndex(BSON("x" << 1));

    runQuery(BSON("x" << BSON("$gte" << 5)));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: {$gte: 5}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

//
// Mod
//

TEST_F(QueryPlannerTest, Mod) {
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{a: {$mod: [2, 0]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$mod: [2, 0]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: {a: {$mod: [2, 0]}}, pattern: {a: 1}}}}}");
}

//
// Exists
//

TEST_F(QueryPlannerTest, ExistsTrue) {
    addIndex(BSON("x" << 1));

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsFalse) {
    addIndex(BSON("x" << 1));

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsTrueSparseIndex) {
    addIndex(BSON("x" << 1), false, true);

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsFalseSparseIndex) {
    addIndex(BSON("x" << 1), false, true);

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ExistsTrueOnUnindexedField) {
    addIndex(BSON("x" << 1));

    runQuery(fromjson("{x: 1, y: {$exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsFalseOnUnindexedField) {
    addIndex(BSON("x" << 1));

    runQuery(fromjson("{x: 1, y: {$exists: false}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsTrueSparseIndexOnOtherField) {
    addIndex(BSON("x" << 1), false, true);

    runQuery(fromjson("{x: 1, y: {$exists: true}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsFalseSparseIndexOnOtherField) {
    addIndex(BSON("x" << 1), false, true);

    runQuery(fromjson("{x: 1, y: {$exists: false}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, ExistsBounds) {
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{b: {$exists: true}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: true}}, node: "
        "{ixscan: {pattern: {b: 1}, bounds: "
        "{b: [['MinKey', 'MaxKey', true, true]]}}}}}");

    // This ends up being a double negation, which we currently don't index.
    runQuery(fromjson("{b: {$not: {$exists: false}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    runQuery(fromjson("{b: {$exists: false}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: false}}, node: "
        "{ixscan: {pattern: {b: 1}, bounds: "
        "{b: [[null, null, true, true]]}}}}}");

    runQuery(fromjson("{b: {$not: {$exists: true}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: false}}, node: "
        "{ixscan: {pattern: {b: 1}, bounds: "
        "{b: [[null, null, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, ExistsBoundsCompound) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuery(fromjson("{a: 1, b: {$exists: true}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: true}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}");

    // This ends up being a double negation, which we currently don't index.
    runQuery(fromjson("{a: 1, b: {$not: {$exists: false}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}");

    runQuery(fromjson("{a: 1, b: {$exists: false}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: false}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [[null,null,true,true]]}}}}}");

    runQuery(fromjson("{a: 1, b: {$not: {$exists: true}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: false}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [[null,null,true,true]]}}}}}");
}

//
// skip and limit
//

TEST_F(QueryPlannerTest, BasicSkipNoIndex) {
    addIndex(BSON("a" << 1));

    runQuerySkipNToReturn(BSON("x" << 5), 3, 0);

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{skip: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
}

TEST_F(QueryPlannerTest, BasicSkipWithIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySkipNToReturn(BSON("a" << 5), 8, 0);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
    assertSolutionExists(
        "{skip: {n: 8, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, BasicLimitNoIndex) {
    addIndex(BSON("a" << 1));

    runQuerySkipNToReturn(BSON("x" << 5), 0, -3);

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{limit: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
}

TEST_F(QueryPlannerTest, BasicSoftLimitNoIndex) {
    addIndex(BSON("a" << 1));

    runQuerySkipNToReturn(BSON("x" << 5), 0, 3);

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {x: 5}}}");
}

TEST_F(QueryPlannerTest, BasicLimitWithIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySkipNToReturn(BSON("a" << 5), 0, -5);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{limit: {n: 5, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
    assertSolutionExists(
        "{limit: {n: 5, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, BasicSoftLimitWithIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySkipNToReturn(BSON("a" << 5), 0, 5);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 5}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, SkipAndLimit) {
    addIndex(BSON("x" << 1));

    runQuerySkipNToReturn(BSON("x" << BSON("$lte" << 4)), 7, -2);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{limit: {n: 2, node: {skip: {n: 7, node: "
        "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}}}");
    assertSolutionExists(
        "{limit: {n: 2, node: {skip: {n: 7, node: {fetch: "
        "{filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, SkipAndSoftLimit) {
    addIndex(BSON("x" << 1));

    runQuerySkipNToReturn(BSON("x" << BSON("$lte" << 4)), 7, 2);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{skip: {n: 7, node: "
        "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}");
    assertSolutionExists(
        "{skip: {n: 7, node: {fetch: "
        "{filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}}}");
}

//
// tree operations
//

TEST_F(QueryPlannerTest, TwoPredicatesAnding) {
    addIndex(BSON("x" << 1));

    runQuery(fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, SimpleOr) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a: 20}, {a: 21}]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {a: 21}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a:1}}}}}");
}

TEST_F(QueryPlannerTest, OrWithoutEnoughIndices) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a: 20}, {b: 21}]}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 20}, {b: 21}]}}}");
}

TEST_F(QueryPlannerTest, OrWithAndChild) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1}}}, "
        "{fetch: {filter: {b: 7}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}}}}}]}}}}");
}

TEST_F(QueryPlannerTest, AndWithUnindexedOrChild) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a:20, $or: [{b:1}, {c:7}]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Logical rewrite means we could get one of these two outcomes:
    size_t matches = 0;
    matches += numSolutionMatches(
        "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    matches += numSolutionMatches(
        "{or: {filter: null, nodes: ["
        "{fetch: {filter: {b:1}, node: {"
        "ixscan: {filter: null, pattern: {a:1}}}}},"
        "{fetch: {filter: {c:7}, node: {"
        "ixscan: {filter: null, pattern: {a:1}}}}}]}}");
    ASSERT_GREATER_THAN_OR_EQUALS(matches, 1U);
}


TEST_F(QueryPlannerTest, AndWithOrWithOneIndex) {
    addIndex(BSON("b" << 1));
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{b:1}, {c:7}], a:20}"));

    // Logical rewrite gives us at least one of these:
    assertSolutionExists("{cscan: {dir: 1}}");
    size_t matches = 0;
    matches += numSolutionMatches(
        "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    matches += numSolutionMatches(
        "{or: {filter: null, nodes: ["
        "{fetch: {filter: {b:1}, node: {"
        "ixscan: {filter: null, pattern: {a:1}}}}},"
        "{fetch: {filter: {c:7}, node: {"
        "ixscan: {filter: null, pattern: {a:1}}}}}]}}");
    ASSERT_GREATER_THAN_OR_EQUALS(matches, 1U);
}

//
// Additional $or tests
//

TEST_F(QueryPlannerTest, OrCollapsesToSingleScan) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:{$gt:2}}, {a:{$gt:0}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, OrCollapsesToSingleScan2) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:{$lt:2}}, {a:{$lt:4}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [[-Infinity,4,true,false]]}}}}}");
}

TEST_F(QueryPlannerTest, OrCollapsesToSingleScan3) {
    addIndex(BSON("a" << 1));
    runQueryHint(fromjson("{$or: [{a:1},{a:3}]}"), fromjson("{a:1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [[1,1,true,true], [3,3,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, OrOnlyOneBranchCanUseIndex) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:1}, {b:2}]}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, OrOnlyOneBranchCanUseIndexHinted) {
    addIndex(BSON("a" << 1));
    runQueryHint(fromjson("{$or: [{a:1}, {b:2}]}"), fromjson("{a:1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {$or:[{a:1},{b:2}]}, node: {ixscan: "
        "{pattern: {a:1}, bounds: "
        "{a: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, OrNaturalHint) {
    addIndex(BSON("a" << 1));
    runQueryHint(fromjson("{$or: [{a:1}, {a:3}]}"), fromjson("{$natural:1}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// SERVER-13714.  A non-top-level indexable negation exposed a bug in plan enumeration.
TEST_F(QueryPlannerTest, NonTopLevelIndexedNegation) {
    addIndex(BSON("state" << 1));
    addIndex(BSON("is_draft" << 1));
    addIndex(BSON("published_date" << 1));
    addIndex(BSON("newsroom_id" << 1));

    BSONObj queryObj = fromjson(
        "{$and:[{$or:[{is_draft:false},{creator_id:1}]},"
        "{$or:[{state:3,is_draft:false},"
        "{published_date:{$ne:null}}]},"
        "{newsroom_id:{$in:[1]}}]}");
    runQuery(queryObj);
}

TEST_F(QueryPlannerTest, NonTopLevelIndexedNegationMinQuery) {
    addIndex(BSON("state" << 1));
    addIndex(BSON("is_draft" << 1));
    addIndex(BSON("published_date" << 1));

    // This is the min query to reproduce SERVER-13714
    BSONObj queryObj = fromjson("{$or:[{state:1, is_draft:1}, {published_date:{$ne: 1}}]}");
    runQuery(queryObj);
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:5}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a:1}, bounds: {a: [[2,10,false,false]]}}}, "
        "{ixscan: {pattern: {a:1}, bounds: "
        "{a: [[0,5,false,false]]}}}]}}}}");
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd2) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:{$gt:2,$lt:10}}, {a:{$gt:0,$lt:15}}, {a:{$gt:20}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a:1}, bounds: {a: [[2,10,false,false]]}}}, "
        "{ixscan: {pattern: {a:1}, bounds: {a: [[0,15,false,false]]}}}, "
        "{ixscan: {pattern: {a:1}, bounds: "
        "{a: [[20,Infinity,false,true]]}}}]}}}}");
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd3) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a:{$gt:1,$lt:5},b:6}, {a:3,b:{$gt:0,$lt:10}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {b:6}, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [[1,5,false,false]]}}}}}, "
        "{fetch: {filter: {$and:[{b:{$lt:10}},{b:{$gt:0}}]}, node: "
        "{ixscan: {pattern: {a:1}, bounds: {a:[[3,3,true,true]]}}}}}]}}");
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd4) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(
        fromjson("{$or: [{a:{$gt:1,$lt:5}, b:{$gt:0,$lt:3}, c:6}, "
                 "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {c:6}, node: {ixscan: {pattern: {a:1,b:1}, "
        "bounds: {a: [[1,5,false,false]], b: [[0,3,false,false]]}}}}}, "
        "{fetch: {filter: {$and:[{c:{$lt:10}},{c:{$gt:0}}]}, node: "
        "{ixscan: {pattern: {a:1,b:1}, "
        " bounds: {a:[[3,3,true,true]], b:[[1,2,false,false]]}}}}}]}}");
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd5) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(
        fromjson("{$or: [{a:{$gt:1,$lt:5}, c:6}, "
                 "{a:3, b:{$gt:1,$lt:2}, c:{$gt:0,$lt:10}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {c:6}, node: {ixscan: {pattern: {a:1,b:1}, "
        "bounds: {a: [[1,5,false,false]], "
        "b: [['MinKey','MaxKey',true,true]]}}}}}, "
        "{fetch: {filter: {$and:[{c:{$lt:10}},{c:{$gt:0}}]}, node: "
        "{ixscan: {pattern: {a:1,b:1}, "
        " bounds: {a:[[3,3,true,true]], b:[[1,2,false,false]]}}}}}]}}");
}

// SERVER-12594: we don't yet collapse an OR of ANDs into a single ixscan.
TEST_F(QueryPlannerTest, OrOfAnd6) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{$or: [{a:{$in:[1]},b:{$in:[1]}}, {a:{$in:[1,5]},b:{$in:[1,5]}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a:1,b:1}, bounds: "
        "{a: [[1,1,true,true]], b: [[1,1,true,true]]}}}, "
        "{ixscan: {pattern: {a:1,b:1}, bounds: "
        "{a: [[1,1,true,true], [5,5,true,true]], "
        " b: [[1,1,true,true], [5,5,true,true]]}}}]}}}}");
}

// We do collapse OR of ANDs if branches of the OR plan are using identical index scans.
TEST_F(QueryPlannerTest, RootedOrOfAndCollapseIndenticalScans) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{$or: [{a:1, b:2}, {a:1, b:2}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, b: 1}},"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]},"
        "filter: null}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOfAndCollapseIndenticalScans) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{c: 1, $or: [{a:1, b:2}, {a:1, b:2}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {a: 1, b: 1}},"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]},"
        "filter: null}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOfAndCollapseIndenticalScansWithFilter) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{c: 1, $or: [{a:1, b:2}, {a:1, b:2, d:3}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {a: 1, b: 1}},"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]},"
        "filter: null}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOfAndCollapseIndenticalScansWithFilter2) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{c: 1, $or: [{a:{$gte:1,$lte:1}, b:2}, {a:1, b:2, d:3}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {fetch: {filter: null, node: "
        "{ixscan: {pattern: {a: 1, b: 1}},"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]},"
        "filter: null}}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOfAndCollapseIdenticalScansTwoFilters) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{c: 1, $or: [{a:1, b:2, d:3}, {a:1, b:2, e:4}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {fetch: {filter: {$or:[{e:4},{d:3}]},"
        "node: {ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, RootedOrOfAndCollapseScansExistingOrFilter) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{$or: [{a:1, b:2, $or: [{c:3}, {d:4}]}, {a:1, b:2, e:5}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{e:5},{c:3},{d:4}]}, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}, "
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, RootedOrOfAndCollapseTwoScansButNotThird) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "d" << 1));
    runQuery(fromjson("{$or: [{a: 1, b: 2}, {c: 3, d: 4}, {a: 1, b: 2}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}},"
        "{ixscan: {pattern: {c: 1, d: 1}, filter: null,"
        "bounds: {c: [[3,3,true,true]], d: [[4,4,true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, RootedOrOfAndCollapseTwoScansButNotThirdWithFilters) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "d" << 1));
    runQuery(fromjson("{$or: [{a:1, b:2, e:5}, {c:3, d:4}, {a:1, b:2, f:6}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {$or: [{f:6},{e:5}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}}}},"
        "{ixscan: {pattern: {c: 1, d: 1}, filter: null,"
        "bounds: {c: [[3,3,true,true]], d: [[4,4,true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, RootedOrOfAndDontCollapseDifferentBounds) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "d" << 1));
    runQuery(fromjson("{$or: [{a: 1, b: 2}, {c: 3, d: 4}, {a: 1, b: 99}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [[99,99,true,true]]}}},"
        "{ixscan: {pattern: {c: 1, d: 1}, filter: null,"
        "bounds: {c: [[3,3,true,true]], d: [[4,4,true,true]]}}}]}}}}");
}

// SERVER-13960: properly handle $or with a mix of exact and inexact predicates.
TEST_F(QueryPlannerTest, OrInexactWithExact) {
    addIndex(BSON("name" << 1));
    runQuery(fromjson("{$or: [{name: 'thomas'}, {name: /^alexand(er|ra)/}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter:"
        "{$or: [{name: 'thomas'}, {name: /^alexand(er|ra)/}]},"
        "pattern: {name: 1}}}}}");
}

// SERVER-13960: multiple indices, each with an inexact covered predicate.
TEST_F(QueryPlannerTest, OrInexactWithExact2) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{$or: [{a: 'foo'}, {a: /bar/}, {b: 'foo'}, {b: /bar/}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {filter: {$or:[{a:'foo'},{a:/bar/}]},"
        "pattern: {a: 1}}},"
        "{ixscan: {filter: {$or:[{b:'foo'},{b:/bar/}]},"
        "pattern: {b: 1}}}]}}}}");
}

// SERVER-13960: an exact, inexact covered, and inexact fetch predicate.
TEST_F(QueryPlannerTest, OrAllThreeTightnesses) {
    addIndex(BSON("names" << 1));
    runQuery(
        fromjson("{$or: [{names: 'frank'}, {names: /^al(ice)|(ex)/},"
                 "{names: {$elemMatch: {$eq: 'thomas'}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: "
        "{$or: [{names: 'frank'}, {names: /^al(ice)|(ex)/},"
        "{names: {$elemMatch: {$eq: 'thomas'}}}]}, "
        "node: {ixscan: {filter: null, pattern: {names: 1}}}}}");
}

// SERVER-13960: two inexact fetch predicates.
TEST_F(QueryPlannerTest, OrTwoInexactFetch) {
    // true means multikey
    addIndex(BSON("names" << 1), true);
    runQuery(
        fromjson("{$or: [{names: {$elemMatch: {$eq: 'alexandra'}}},"
                 "{names: {$elemMatch: {$eq: 'thomas'}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: "
        "{$or: [{names: {$elemMatch: {$eq: 'alexandra'}}},"
        "{names: {$elemMatch: {$eq: 'thomas'}}}]}, "
        "node: {ixscan: {filter: null, pattern: {names: 1}}}}}");
}

// SERVER-13960: multikey with exact and inexact covered predicates.
TEST_F(QueryPlannerTest, OrInexactCoveredMultikey) {
    // true means multikey
    addIndex(BSON("names" << 1), true);
    runQuery(fromjson("{$or: [{names: 'dave'}, {names: /joe/}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{names: 'dave'}, {names: /joe/}]}, "
        "node: {ixscan: {filter: null, pattern: {names: 1}}}}}");
}

// SERVER-13960: $elemMatch object with $or.
TEST_F(QueryPlannerTest, OrElemMatchObject) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(
        fromjson("{$or: [{a: {$elemMatch: {b: {$lte: 1}}}},"
                 "{a: {$elemMatch: {b: {$gte: 4}}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:4}}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}},"
        "{fetch: {filter: {a:{$elemMatch:{b:{$lte:1}}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}]}}");
}

// SERVER-13960: $elemMatch object inside an $or, below an AND.
TEST_F(QueryPlannerTest, OrElemMatchObjectBeneathAnd) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(
        fromjson("{$or: [{'a.b': 0, a: {$elemMatch: {b: {$lte: 1}}}},"
                 "{a: {$elemMatch: {b: {$gte: 4}}}}]}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {$and:[{a:{$elemMatch:{b:{$lte:1}}}},{'a.b':0}]},"
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, "
        "bounds: {'a.b': [[-Infinity,1,true,true]]}}}}},"
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:4}}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1},"
        "bounds: {'a.b': [[4,Infinity,true,true]]}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {a:{$elemMatch:{b:{$lte:1}}}},"
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, "
        "bounds: {'a.b': [[0,0,true,true]]}}}}},"
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:4}}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1},"
        "bounds: {'a.b': [[4,Infinity,true,true]]}}}}}]}}");
}

// SERVER-13960: $or below $elemMatch with an inexact covered predicate.
TEST_F(QueryPlannerTest, OrBelowElemMatchInexactCovered) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {$or: [{b: 'x'}, {b: /z/}]}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$or: [{b: 'x'}, {b: /z/}]}}},"
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

// SERVER-13960: $in with exact and inexact covered predicates.
TEST_F(QueryPlannerTest, OrWithExactAndInexact) {
    addIndex(BSON("name" << 1));
    runQuery(fromjson("{name: {$in: ['thomas', /^alexand(er|ra)/]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: {name: {$in: ['thomas', /^alexand(er|ra)/]}}, "
        "pattern: {name: 1}}}}}");
}

// SERVER-13960: $in with exact, inexact covered, and inexact fetch predicates.
TEST_F(QueryPlannerTest, OrWithExactAndInexact2) {
    addIndex(BSON("name" << 1));
    runQuery(
        fromjson("{$or: [{name: {$in: ['thomas', /^alexand(er|ra)/]}},"
                 "{name: {$exists: false}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{name: {$in: ['thomas', /^alexand(er|ra)/]}},"
        "{name: {$exists: false}}]}, "
        "node: {ixscan: {filter: null, pattern: {name: 1}}}}}");
}

// SERVER-13960: $in with exact, inexact covered, and inexact fetch predicates
// over two indices.
TEST_F(QueryPlannerTest, OrWithExactAndInexact3) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(
        fromjson("{$or: [{a: {$in: [/z/, /x/]}}, {a: 'w'},"
                 "{b: {$exists: false}}, {b: {$in: ['p']}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: {$or:[{a:{$in:[/z/, /x/]}}, {a:'w'}]}, "
        "pattern: {a: 1}}}, "
        "{fetch: {filter: {$or:[{b:{$exists:false}}, {b:{$eq:'p'}}]},"
        "node: {ixscan: {filter: null, pattern: {b: 1}}}}}]}}}}");
}

//
// Min/Max
//

TEST_F(QueryPlannerTest, MinValid) {
    addIndex(BSON("a" << 1));
    runQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MinWithoutIndex) {
    runInvalidQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());
}

TEST_F(QueryPlannerTest, MinBadHint) {
    addIndex(BSON("b" << 1));
    runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), fromjson("{a: 1}"), BSONObj());
}

TEST_F(QueryPlannerTest, MaxValid) {
    addIndex(BSON("a" << 1));
    runQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MinMaxSameValue) {
    addIndex(BSON("a" << 1));
    runQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxWithoutIndex) {
    runInvalidQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));
}

TEST_F(QueryPlannerTest, MaxBadHint) {
    addIndex(BSON("b" << 1));
    runInvalidQueryHintMinMax(BSONObj(), fromjson("{b: 1}"), BSONObj(), fromjson("{a: 1}"));
}

TEST_F(QueryPlannerTest, MaxMinSort) {
    addIndex(BSON("a" << 1));

    // Run an empty query, sort {a: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{a: 1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 2}"),
                 fromjson("{a: 8}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinSortEqualityFirstSortSecond) {
    addIndex(BSON("a" << 1 << "b" << 1));

    // Run an empty query, sort {b: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{b: 1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 1, b: 2}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinSortInequalityFirstSortSecond) {
    addIndex(BSON("a" << 1 << "b" << 1));

    // Run an empty query, sort {b: 1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{b: 1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 1, b: 1}"),
                 fromjson("{a: 2, b: 2}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseSort) {
    addIndex(BSON("a" << 1));

    // Run an empty query, sort {a: -1}, max/min arguments.
    runQueryFull(BSONObj(),
                 fromjson("{a: -1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 2}"),
                 fromjson("{a: 8}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: -1, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseIndexDir) {
    addIndex(BSON("a" << -1));

    // Because the index is descending, the min is numerically larger than the max.
    runQueryFull(BSONObj(),
                 fromjson("{a: -1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 8}"),
                 fromjson("{a: 2}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: 1, pattern: {a: -1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinReverseIndexDirSort) {
    addIndex(BSON("a" << -1));

    // Min/max specifies a forward scan with bounds [{a: 8}, {a: 2}]. Asking for
    // an ascending sort reverses the direction of the scan to [{a: 2}, {a: 8}].
    runQueryFull(BSONObj(),
                 fromjson("{a: 1}"),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 8}"),
                 fromjson("{a: 2}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter: null, dir: -1,"
        "pattern: {a: -1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinNoMatchingIndexDir) {
    addIndex(BSON("a" << -1));
    runInvalidQueryHintMinMax(BSONObj(), fromjson("{a: 2}"), BSONObj(), fromjson("{a: 8}"));
}

TEST_F(QueryPlannerTest, MaxMinSelectCorrectlyOrderedIndex) {
    // There are both ascending and descending indices on 'a'.
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << -1));

    // The ordering of min and max means that we *must* use the descending index.
    runQueryFull(BSONObj(),
                 BSONObj(),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 8}"),
                 fromjson("{a: 2}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: 1, pattern: {a: -1}}}}}");

    // If we switch the ordering, then we use the ascending index.
    // The ordering of min and max means that we *must* use the descending index.
    runQueryFull(BSONObj(),
                 BSONObj(),
                 BSONObj(),
                 0,
                 0,
                 BSONObj(),
                 fromjson("{a: 2}"),
                 fromjson("{a: 8}"),
                 false);

    assertNumSolutions(1);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, dir: 1, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, MaxMinBadHintSelectsReverseIndex) {
    // There are both ascending and descending indices on 'a'.
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << -1));

    // A query hinting on {a: 1} is bad if min is {a: 8} and {a: 2} because this
    // min/max pairing requires a descending index.
    runInvalidQueryFull(BSONObj(),
                        BSONObj(),
                        BSONObj(),
                        0,
                        0,
                        fromjson("{a: 1}"),
                        fromjson("{a: 8}"),
                        fromjson("{a: 2}"),
                        false);
}


//
// snapshot
//

TEST_F(QueryPlannerTest, Snapshot) {
    addIndex(BSON("a" << 1));
    runQuerySnapshot(fromjson("{a: {$gt: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {filter: {a: {$gt: 0}}, dir: 1}}");
}

TEST_F(QueryPlannerTest, SnapshotUseId) {
    params.options = QueryPlannerParams::SNAPSHOT_USE_ID;

    addIndex(BSON("a" << 1));
    runQuerySnapshot(fromjson("{a: {$gt: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:0}}, node: "
        "{ixscan: {filter: null, pattern: {_id: 1}}}}}");
}

TEST_F(QueryPlannerTest, CannotSnapshotWithGeoNear) {
    // Snapshot is skipped with geonear queries.
    addIndex(BSON("a"
                  << "2d"));
    runQuerySnapshot(fromjson("{a: {$near: [0,0]}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{geoNear2d: {a: '2d'}}");
}

//
// Tree operations that require simple tree rewriting.
//

TEST_F(QueryPlannerTest, AndOfAnd) {
    addIndex(BSON("x" << 1));
    runQuery(fromjson("{$and: [ {$and: [ {x: 2.5}]}, {x: {$gt: 1}}, {x: {$lt: 3}} ] }"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

//
// Logically equivalent queries
//

TEST_F(QueryPlannerTest, EquivalentAndsOne) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{$and: [{a: 1}, {b: {$all: [10, 20]}}]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{b:10},{b:20}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, EquivalentAndsTwo) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{$and: [{a: 1, b: 10}, {a: 1, b: 20}]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{a:1},{a:1},{b:10},{b:20}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}");
}

//
// Covering
//

TEST_F(QueryPlannerTest, BasicCovering) {
    addIndex(BSON("x" << 1));
    // query, sort, proj
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: "
        "{cscan: {dir: 1, filter: {x:{$gt:1}}}}}}");
}

TEST_F(QueryPlannerTest, DottedFieldCovering) {
    addIndex(BSON("a.b" << 1));
    runQuerySortProj(fromjson("{'a.b': 5}"), BSONObj(), fromjson("{_id: 0, 'a.b': 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: "
        "{cscan: {dir: 1, filter: {'a.b': 5}}}}}");
    // SERVER-2104
    // assertSolutionExists("{proj: {spec: {_id: 0, 'a.b': 1}, node: {'a.b': 1}}}");
}

TEST_F(QueryPlannerTest, IdCovering) {
    runQuerySortProj(fromjson("{_id: {$gt: 10}}"), BSONObj(), fromjson("{_id: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node: "
        "{cscan: {dir: 1, filter: {_id: {$gt: 10}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node: {ixscan: "
        "{filter: null, pattern: {_id: 1}}}}}");
}

TEST_F(QueryPlannerTest, ProjNonCovering) {
    addIndex(BSON("x" << 1));
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{x: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {x: 1}, node: {cscan: "
        "{dir: 1, filter: {x: {$gt: 1}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {x: 1}, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {x: 1}}}}}}}");
}

//
// Basic sort
//

TEST_F(QueryPlannerTest, BasicSort) {
    addIndex(BSON("x" << 1));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseHashedIndexToProvideSort) {
    addIndex(BSON("x"
                  << "hashed"));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseHashedIndexToProvideSortWithIndexablePred) {
    addIndex(BSON("x"
                  << "hashed"));
    runQuerySortProj(BSON("x" << BSON("$in" << BSON_ARRAY(0 << 1))), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {x: 'hashed'}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: {node:"
        "{cscan: {dir: 1, filter: {x: {$in: [0, 1]}}}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseTextIndexToProvideSort) {
    addIndex(BSON("x" << 1 << "_fts"
                      << "text"
                      << "_ftsx"
                      << 1));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, BasicSortWithIndexablePred) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {a: 5}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {ixscan: "
        "{filter: null, pattern: {b: 1}}}}}");
}

TEST_F(QueryPlannerTest, BasicSortBooleanIndexKeyPattern) {
    addIndex(BSON("a" << true));
    runQuerySortProj(fromjson("{ a : 5 }"), BSON("a" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {a: 5}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: true}}}}}");
}

// SERVER-14070
TEST_F(QueryPlannerTest, CompoundIndexWithEqualityPredicatesProvidesSort) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: 1, b: 1}"), fromjson("{b: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null,"
        "pattern: {a: 1, b: 1}, "
        "bounds: {a:[[1,1,true,true]], b:[[1,1,true,true]]}}}}}");
}

//
// Sort with limit and/or skip
//

TEST_F(QueryPlannerTest, SortLimit) {
    // Negative limit indicates hard limit - see query_request.cpp
    runQuerySortProjSkipNToReturn(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, -3);
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 3, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, SortSkip) {
    runQuerySortProjSkipNToReturn(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 0);
    assertNumSolutions(1U);
    // If only skip is provided, do not limit sort.
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, SortSkipLimit) {
    runQuerySortProjSkipNToReturn(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, -3);
    assertNumSolutions(1U);
    // Limit in sort node should be adjusted by skip count
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 5, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, SortSoftLimit) {
    runQuerySortProjSkipNToReturn(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, 3);
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 3, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, SortSkipSoftLimit) {
    runQuerySortProjSkipNToReturn(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 3);
    assertNumSolutions(1U);
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 5, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}}}");
}

//
// Sort elimination
//

TEST_F(QueryPlannerTest, BasicSortElim) {
    addIndex(BSON("x" << 1));
    // query, sort, proj
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), fromjson("{x: 1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {x: {$gt: 1}}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, SortElimCompound) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {a: 5}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}");
}

// SERVER-13611: test that sort elimination still works if there are
// trailing fields in the index.
TEST_F(QueryPlannerTest, SortElimTrailingFields) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuerySortProj(fromjson("{a: 5}"), BSON("b" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {a: 5}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1, c: 1}}}}}");
}

// Sort elimination with trailing fields where the sort direction is descending.
TEST_F(QueryPlannerTest, SortElimTrailingFieldsReverse) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
    runQuerySortProj(fromjson("{a: 5, b: 6}"), BSON("c" << -1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {c: -1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {a: 5, b: 6}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, dir: -1, pattern: {a: 1, b: 1, c: 1, d: 1}}}}}");
}

//
// Basic compound
//

TEST_F(QueryPlannerTest, BasicCompound) {
    addIndex(BSON("x" << 1 << "y" << 1));
    runQuery(fromjson("{ x : 5, y: 10}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1, y: 1}}}}}");
}

TEST_F(QueryPlannerTest, CompoundMissingField) {
    addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
    runQuery(fromjson("{ x : 5, z: 10}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
}

TEST_F(QueryPlannerTest, CompoundFieldsOrder) {
    addIndex(BSON("x" << 1 << "y" << 1 << "z" << 1));
    runQuery(fromjson("{ x : 5, z: 10, y:1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1, y: 1, z: 1}}}}}");
}

TEST_F(QueryPlannerTest, CantUseCompound) {
    addIndex(BSON("x" << 1 << "y" << 1));
    runQuery(fromjson("{ y: 10}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {y: 10}}}");
}

//
// $in
//

TEST_F(QueryPlannerTest, InBasic) {
    addIndex(fromjson("{a: 1}"));
    runQuery(fromjson("{a: {$in: [1, 2]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$in: [1, 2]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {pattern: {a: 1}}}}}");
}

// Logically equivalent to the preceding $in query.
// Indexed solution should be the same.
TEST_F(QueryPlannerTest, InBasicOrEquivalent) {
    addIndex(fromjson("{a: 1}"));
    runQuery(fromjson("{$or: [{a: 1}, {a: 2}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a: 1}, {a: 2}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, InSparseIndex) {
    addIndex(fromjson("{a: 1}"),
             false,  // multikey
             true);  // sparse
    runQuery(fromjson("{a: {$in: [null]}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$eq: null}}}}");
}

TEST_F(QueryPlannerTest, InCompoundIndexFirst) {
    addIndex(fromjson("{a: 1, b: 1}"));
    runQuery(fromjson("{a: {$in: [1, 2]}, b: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {b: 3, a: {$in: [1, 2]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}

// Logically equivalent to the preceding $in query.
// Indexed solution should be the same.
// Currently fails - pre-requisite to SERVER-12024
/*
TEST_F(QueryPlannerTest, InCompoundIndexFirstOrEquivalent) {
    addIndex(fromjson("{a: 1, b: 1}"));
    runQuery(fromjson("{$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}}}");
    assertSolutionExists("{fetch: {filter: null, "
                         "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}
*/

TEST_F(QueryPlannerTest, InCompoundIndexLast) {
    addIndex(fromjson("{a: 1, b: 1}"));
    runQuery(fromjson("{a: 3, b: {$in: [1, 2]}}"));

    assertNumSolutions(2U);
    // TODO: update filter in cscan solution when SERVER-12024 is implemented
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 3, b: {$in: [1, 2]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}

// Logically equivalent to the preceding $in query.
// Indexed solution should be the same.
// Currently fails - pre-requisite to SERVER-12024
/*
TEST_F(QueryPlannerTest, InCompoundIndexLastOrEquivalent) {
    addIndex(fromjson("{a: 1, b: 1}"));
    runQuery(fromjson("{$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{a: 3}, {$or: [{b: 1}, {b: 2}]}]}}}");
    assertSolutionExists("{fetch: {filter: null, "
                         "node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}
*/

// SERVER-1205
TEST_F(QueryPlannerTest, InWithSort) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1, 2]}}"), BSON("b" << 1), BSONObj(), 0, 1);

    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 1, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, b: 1}}}, {ixscan: {pattern: {a: 1, b: 1}}}]}}}}");
}

// SERVER-1205
TEST_F(QueryPlannerTest, InWithoutSort) {
    addIndex(BSON("a" << 1 << "b" << 1));
    // No sort means we don't bother to blow up the bounds.
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1, 2]}}"), BSONObj(), BSONObj(), 0, 1);

    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}

// SERVER-1205
TEST_F(QueryPlannerTest, ManyInWithSort) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1, 2]}, b:{$in:[1,2]}, c:{$in:[1,2]}}"),
                                  BSON("d" << 1),
                                  BSONObj(),
                                  0,
                                  1);

    assertSolutionExists(
        "{sort: {pattern: {d: 1}, limit: 1, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}}]}}}}");
}

// SERVER-1205
TEST_F(QueryPlannerTest, TooManyToExplode) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1,2,3,4,5,6]},"
                                           "b:{$in:[1,2,3,4,5,6,7,8]},"
                                           "c:{$in:[1,2,3,4,5,6,7,8]}}"),
                                  BSON("d" << 1),
                                  BSONObj(),
                                  0,
                                  1);

    // We cap the # of ixscans we're willing to create.
    assertNumSolutions(2);
    assertSolutionExists(
        "{sort: {pattern: {d: 1}, limit: 1, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {d: 1}, limit: 1, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c:1, d:1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CantExplodeMetaSort) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c"
                      << "text"));
    runQuerySortProj(fromjson("{a: {$in: [1, 2]}, b: {$in: [3, 4]}}"),
                     fromjson("{c: {$meta: 'textScore'}}"),
                     fromjson("{c: {$meta: 'textScore'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {c:{$meta:'textScore'}}, node: "
        "{sort: {pattern: {c:{$meta:'textScore'}}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {filter: {a:{$in:[1,2]},b:{$in:[3,4]}}, dir: 1}}}}}}}}");
}

// SERVER-13618: test that exploding scans for sort works even
// if we must reverse the scan direction.
TEST_F(QueryPlannerTest, ExplodeMustReverseScans) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
    runQuerySortProj(fromjson("{a: {$in: [1, 2]}, b: {$in: [3, 4]}}"), BSON("c" << -1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: -1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a:1, b:1, c:1, d:1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:1, d:1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:1, d:1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:1, d:1}}}]}}}}");
}

// SERVER-13618
TEST_F(QueryPlannerTest, ExplodeMustReverseScans2) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << -1));
    runQuerySortProj(fromjson("{a: {$in: [1, 2]}, b: {$in: [3, 4]}}"), BSON("c" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a:1, b:1, c:-1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:-1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:-1}}},"
        "{ixscan: {pattern: {a:1, b:1, c:-1}}}]}}}}");
}

// SERVER-13752: don't try to explode if the ordered interval list for
// the leading field of the compound index is empty.
TEST_F(QueryPlannerTest, CantExplodeWithEmptyBounds) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: {$in: []}}"), BSON("b" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
}

// SERVER-13752
TEST_F(QueryPlannerTest, CantExplodeWithEmptyBounds2) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuerySortProj(fromjson("{a: {$gt: 3, $lt: 0}}"), BSON("b" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a:1,b:1,c:1}}}}}}}}}");
}

// SERVER-13754: exploding an $or
TEST_F(QueryPlannerTest, ExplodeOrForSort) {
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("b" << 1 << "c" << 1));

    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}, {b: 2}]}"), BSON("c" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {bounds: {a: [[1,1,true,true]], "
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {a:1, c:1}}},"
        "{ixscan: {bounds: {a: [[2,2,true,true]], "
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {a:1, c:1}}},"
        "{ixscan: {bounds: {b: [[2,2,true,true]], "
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {b:1, c:1}}}]}}}}");
}

// SERVER-13754: exploding an $or
TEST_F(QueryPlannerTest, ExplodeOrForSort2) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("d" << 1 << "c" << 1));

    runQuerySortProj(
        fromjson("{$or: [{a: 1, b: {$in: [1, 2]}}, {d: 3}]}"), BSON("c" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {bounds: {a: [[1,1,true,true]], b: [[1,1,true,true]],"
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {a:1, b:1, c:1}}},"
        "{ixscan: {bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]],"
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {a:1, b:1, c:1}}},"
        "{ixscan: {bounds: {d: [[3,3,true,true]], "
        "c: [['MinKey','MaxKey',true,true]]},"
        "pattern: {d:1, c:1}}}]}}}}");
}

// SERVER-13754: an $or that can't be exploded, because one clause of the
// $or does provide the sort, even after explosion.
TEST_F(QueryPlannerTest, CantExplodeOrForSort) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("d" << 1 << "c" << 1));

    runQuerySortProj(fromjson("{$or: [{a: {$in: [1, 2]}}, {d: 3}]}"), BSON("c" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}}},"
        "{ixscan: {pattern: {d: 1, c: 1}}}]}}}}}}}}");
}

// SERVER-13754: too many scans in an $or explosion.
TEST_F(QueryPlannerTest, TooManyToExplodeOr) {
    addIndex(BSON("a" << 1 << "e" << 1));
    addIndex(BSON("b" << 1 << "e" << 1));
    addIndex(BSON("c" << 1 << "e" << 1));
    addIndex(BSON("d" << 1 << "e" << 1));
    runQuerySortProj(fromjson("{$or: [{a: {$in: [1,2,3,4,5,6]},"
                              "b: {$in: [1,2,3,4,5,6]}},"
                              "{c: {$in: [1,2,3,4,5,6]},"
                              "d: {$in: [1,2,3,4,5,6]}}]}"),
                     BSON("e" << 1),
                     BSONObj());

    // We cap the # of ixscans we're willing to create, so we don't get explosion. Instead
    // we get 5 different solutions which all use a blocking sort.
    assertNumSolutions(5U);
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{or: {nodes: ["
        "{fetch: {node: {ixscan: {pattern: {a: 1, e: 1}}}}},"
        "{fetch: {node: {ixscan: {pattern: {c: 1, e: 1}}}}}]}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{or: {nodes: ["
        "{fetch: {node: {ixscan: {pattern: {b: 1, e: 1}}}}},"
        "{fetch: {node: {ixscan: {pattern: {c: 1, e: 1}}}}}]}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{or: {nodes: ["
        "{fetch: {node: {ixscan: {pattern: {a: 1, e: 1}}}}},"
        "{fetch: {node: {ixscan: {pattern: {d: 1, e: 1}}}}}]}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{or: {nodes: ["
        "{fetch: {node: {ixscan: {pattern: {b: 1, e: 1}}}}},"
        "{fetch: {node: {ixscan: {pattern: {d: 1, e: 1}}}}}]}}}}}}");
}

// SERVER-15696: Make sure explodeForSort copies filters on IXSCAN stages to all of the
// scans resulting from the explode. Regex is the easiest way to have the planner create
// an index scan which filters using the index key.
TEST_F(QueryPlannerTest, ExplodeIxscanWithFilter) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySortProj(fromjson("{$and: [{b: {$regex: 'foo', $options: 'i'}},"
                              "{a: {$in: [1, 2]}}]}"),
                     BSON("b" << 1),
                     BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a:1, b:1},"
        "filter: {b: {$regex: 'foo', $options: 'i'}}}},"
        "{ixscan: {pattern: {a:1, b:1},"
        "filter: {b: {$regex: 'foo', $options: 'i'}}}}]}}}}");
}

TEST_F(QueryPlannerTest, InWithSortAndLimitTrailingField) {
    addIndex(BSON("a" << 1 << "b" << -1 << "c" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1, 2]}, b: {$gte: 0}}"),
                                  fromjson("{b: -1}"),
                                  BSONObj(),  // no projection
                                  0,          // no skip
                                  -1);        // .limit(1)

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b:-1}, limit: 1, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{limit: {n: 1, node: {fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a:1,b:-1,c:1}}}, "
        " {ixscan: {pattern: {a:1,b:-1,c:1}}}]}}}}}}");
}

TEST_F(QueryPlannerTest, InCantUseHashedIndexWithRegex) {
    addIndex(BSON("a"
                  << "hashed"));
    runQuery(fromjson("{a: {$in: [/abc/]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
}

TEST_F(QueryPlannerTest, ExplodeForSortWorksWithShardingFilter) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("c" << 1);

    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: {$in: [1, 3]}}"), fromjson("{b: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: {fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan:  {pattern: {a:1,b:1}, filter: null, bounds: {a: [[1,1,true,true]], b: "
        "[['MinKey','MaxKey',true,true]]}}},"
        "{ixscan:  {pattern: {a:1,b:1}, filter: null, bounds: {a: [[3,3,true,true]], b: "
        "[['MinKey','MaxKey',true,true]]}}}]}}}}}}");
}

TEST_F(QueryPlannerTest, ExplodeRootedOrForSortWorksWithShardingFilter) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("c" << 1);

    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 3}]}"), fromjson("{b: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: {fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan:  {pattern: {a:1,b:1}, filter: null, bounds: {a: [[1,1,true,true]], b: "
        "[['MinKey','MaxKey',true,true]]}}},"
        "{ixscan:  {pattern: {a:1,b:1}, filter: null, bounds: {a: [[3,3,true,true]], b: "
        "[['MinKey','MaxKey',true,true]]}}}]}}}}}}");
}

//
// Multiple solutions
//

TEST_F(QueryPlannerTest, TwoPlans) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuery(fromjson("{a:1, b:{$gt:2,$lt:2}}"));

    // 2 indexed solns and one non-indexed
    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and:[{b:{$lt:2}},{a:1},{b:{$gt:2}}]}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, TwoPlansElemMatch) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("arr.x" << 1 << "a" << 1));

    runQuery(
        fromjson("{arr: { $elemMatch : { x : 5 , y : 5 } },"
                 " a : 55 , b : { $in : [ 1 , 5 , 8 ] } }"));

    // 2 indexed solns and one non-indexed
    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[55,55,true,true]], b: [[1,1,true,true], "
        "[5,5,true,true], [8,8,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{arr:{$elemMatch:{x:5,y:5}}},"
        "{b:{$in:[1,5,8]}}]}, "
        "node: {ixscan: {pattern: {'arr.x':1,a:1}, bounds: "
        "{'arr.x': [[5,5,true,true]], 'a':[[55,55,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundAndNonCompoundIndices) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1), true);
    runQuery(fromjson("{a: 1, b: {$gt: 2, $lt: 2}}"));

    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{b:{$lt:2}},{b:{$gt:2}}]}, node: "
        "{ixscan: {pattern: {a:1}, bounds: {a: [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt:2}}, node: "
        "{ixscan: {pattern: {a:1,b:1}, bounds: "
        "{a: [[1,1,true,true]], b: [[-Infinity,2,true,false]]}}}}}");
}

//
// Sort orders
//

// SERVER-1205.
TEST_F(QueryPlannerTest, MergeSort) {
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("b" << 1 << "c" << 1));
    runQuerySortProj(fromjson("{$or: [{a:1}, {b:1}]}"), fromjson("{c:1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, c: 1}}}, {ixscan: {pattern: {b: 1, c: 1}}}]}}}}");
}

// SERVER-1205 as well.
TEST_F(QueryPlannerTest, NoMergeSortIfNoSortWanted) {
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("b" << 1 << "c" << 1));
    runQuerySortProj(fromjson("{$or: [{a:1}, {b:1}]}"), BSONObj(), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$or: [{a:1}, {b:1}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1, c: 1}}}, "
        "{ixscan: {filter: null, pattern: {b: 1, c: 1}}}]}}}}");
}

// Basic "keep sort in mind with an OR"
TEST_F(QueryPlannerTest, MergeSortEvenIfSameIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{$or: [{a:1}, {a:7}]}"), fromjson("{b:1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    // TODO the second solution should be mergeSort rather than just sort
}

TEST_F(QueryPlannerTest, ReverseScanForSort) {
    addIndex(BSON("_id" << 1));
    runQuerySortProj(BSONObj(), fromjson("{_id: -1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {_id: -1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {_id: 1}}}}}");
}

TEST_F(QueryPlannerTest, MergeSortReverseScans) {
    addIndex(BSON("a" << 1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 1, b: 1}, {a: {$lt: 0}}]}, sort: {a: -1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: -1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}, dir: -1}}}}, {ixscan: "
        "{pattern: {a: 1}, dir: -1}}]}}}}");
}

TEST_F(QueryPlannerTest, MergeSortReverseScanOneIndex) {
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("b" << 1 << "c" << -1));
    runQueryAsCommand(fromjson("{find: 'testns', filter: {$or: [{a: 1}, {b: 1}]}, sort: {c: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, c: 1}, dir: 1}}, {ixscan: {pattern: {b: 1, c: -1}, dir: "
        "-1}}]}}}}");
}

TEST_F(QueryPlannerTest, MergeSortReverseScanOneIndexNotExplodeForSort) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << -1 << "b" << -1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 1, b: 1}, {a: {$lt: 0}}]}, sort: {a: -1}}"));

    assertNumSolutions(5U);
    assertSolutionExists(
        "{sort: {pattern: {a: -1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: -1, b: -1}, dir: 1}}, {ixscan: {pattern: {a: 1}, dir: -1}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}, dir: -1}}}}, {ixscan: "
        "{pattern: {a: 1}, dir: -1}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}, dir: -1}}}}, {ixscan: "
        "{pattern: {a: -1, b: -1}, dir: 1}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: -1, b: -1}, dir: 1}}, {ixscan: {pattern: {a: -1, b: -1}, dir: "
        "1}}]}}}}");
}

TEST_F(QueryPlannerTest, MergeSortReverseIxscanBelowFetch) {
    addIndex(BSON("a" << 1 << "d" << 1));
    addIndex(BSON("b" << 1 << "d" << -1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 1}, {b: 1, c: 1}]}, sort: {d: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {d: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, d: 1}, dir: 1}}, {fetch: {node: {ixscan: {pattern: {b: 1, d: "
        "-1}, dir: -1}}}}]}}}}");
}

TEST_F(QueryPlannerTest, MergeSortReverseSubtreeContainedOr) {
    addIndex(BSON("a" << 1 << "e" << 1));
    addIndex(BSON("c" << 1 << "e" << -1));
    addIndex(BSON("d" << 1 << "e" << -1));
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {$or: [{a: 1}, {b: 1, $or: [{c: 1}, {d: 1}]}]}, sort: {e: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {e: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, e: 1}, dir: 1}}, {fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {c: 1, e: -1}, dir: -1}}, {ixscan: {pattern: {d: 1, e: -1}, dir: "
        "-1}}]}}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotMergeSort) {
    addIndex(BSON("a" << 1 << "c" << -1));
    addIndex(BSON("b" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', filter: {$or: [{a: 1}, {b: 1}]}, sort: {c: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {or: {nodes: [{ixscan: {pattern: {a: 1, c: -1}, dir: -1}}, {ixscan: "
        "{pattern: {b: 1}, dir: 1}}]}}}}}}}}");
}

//
// Hint tests
//

TEST_F(QueryPlannerTest, NaturalHint) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortHint(BSON("a" << 1), BSON("b" << 1), BSON("$natural" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {filter: {a: 1}, dir: 1}}}}}}");
}

// Test $natural sort and its interaction with $natural hint.
TEST_F(QueryPlannerTest, NaturalSortAndHint) {
    addIndex(BSON("x" << 1));

    // Non-empty query, -1 sort, no hint.
    runQuerySortHint(fromjson("{x: {$exists: true}}"), BSON("$natural" << -1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Non-empty query, 1 sort, no hint.
    runQuerySortHint(fromjson("{x: {$exists: true}}"), BSON("$natural" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Non-empty query, -1 sort, -1 hint.
    runQuerySortHint(
        fromjson("{x: {$exists: true}}"), BSON("$natural" << -1), BSON("$natural" << -1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Non-empty query, 1 sort, 1 hint.
    runQuerySortHint(
        fromjson("{x: {$exists: true}}"), BSON("$natural" << 1), BSON("$natural" << 1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Empty query, -1 sort, no hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << -1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Empty query, 1 sort, no hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // Empty query, -1 sort, -1 hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << -1), BSON("$natural" << -1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: -1}}");

    // Empty query, 1 sort, 1 hint.
    runQuerySortHint(BSONObj(), BSON("$natural" << 1), BSON("$natural" << 1));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, HintValid) {
    addIndex(BSON("a" << 1));
    runQueryHint(BSONObj(), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintValidWithPredicate) {
    addIndex(BSON("a" << 1));
    runQueryHint(fromjson("{a: {$gt: 1}}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintValidWithSort) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortHint(fromjson("{a: 100, b: 200}"), fromjson("{b: 1}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {filter: {b: 200}, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, HintElemMatch) {
    // true means multikey
    addIndex(fromjson("{'a.b': 1}"), true);
    runQueryHint(fromjson("{'a.b': 1, a: {$elemMatch: {b: 2}}}"), fromjson("{'a.b': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:2}}}, {'a.b': 1}]}, "
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[2, 2, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:2}}}, "
        "node: {ixscan: {filter: null, pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[1, 1, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, HintInvalid) {
    addIndex(BSON("a" << 1));
    runInvalidQueryHint(BSONObj(), fromjson("{b: 1}"));
}

TEST_F(QueryPlannerTest, HintedBlockingSortIndexFilteredOut) {
    params.options = QueryPlannerParams::NO_BLOCKING_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 1}, sort: {b: 1}, hint: {a: 1}}"));
    assertNumSolutions(0U);
}

TEST_F(QueryPlannerTest, HintedNotCoveredProjectionIndexFilteredOut) {
    params.options = QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: 1}, projection: {a: 1, b: 1, _id: 0}, hint: {a: 1}}"));
    assertNumSolutions(0U);
}

//
// Sparse indices, SERVER-8067
// Each index in this block of tests is sparse.
//

TEST_F(QueryPlannerTest, SparseIndexIgnoreForSort) {
    addIndex(fromjson("{a: 1}"), false, true);
    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, SparseIndexHintForSort) {
    addIndex(fromjson("{a: 1}"), false, true);
    runQuerySortHint(BSONObj(), fromjson("{a: 1}"), fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, SparseIndexPreferCompoundIndexForSort) {
    addIndex(fromjson("{a: 1}"), false, true);
    addIndex(fromjson("{a: 1, b: 1}"));
    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, SparseIndexForQuery) {
    addIndex(fromjson("{a: 1}"), false, true);
    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 1}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1}}}}}");
}

//
// Regex
//

TEST_F(QueryPlannerTest, PrefixRegex) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: /^foo/}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: /^foo/}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, PrefixRegexCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: /^foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {a: /^foo/}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegex) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: /foo/}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: /foo/}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: /foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {a: /foo/}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {a: /foo/}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexAnd) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: /foo/, b: 2}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexAndCovering) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: /foo/, b: 2}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{ixscan: {filter: {a: /foo/}, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexOrCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$or: [{a: /0/}, {a: /1/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {$or: [{a: /0/}, {a: /1/}]}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {$or: [{a: /0/}, {a: /1/}]}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexInCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$in: [/foo/, /bar/]}}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {a:{$in:[/foo/,/bar/]}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {a:{$in:[/foo/,/bar/]}}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, TwoRegexCompoundIndexCovering) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: /0/, b: /1/}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{b:/1/}]}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{ixscan: {filter: {$and:[{a:/0/},{b:/1/}]}, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, TwoRegexSameFieldCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$and: [{a: /0/}, {a: /1/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/}]}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {$and:[{a:/0/},{a:/1/}]}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, ThreeRegexSameFieldCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexMultikey) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: /foo/}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {filter: {a: /foo/}, dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: /foo/}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null}}}}");
}

TEST_F(QueryPlannerTest, ThreeRegexSameFieldMultikey) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"));

    ASSERT_EQUALS(getNumSolutions(), 4U);
    assertSolutionExists("{cscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null, "
        "bounds: {a: [['', {}, true, false], [/0/, /0/, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:/1/},{a:/0/},{a:/2/}]}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null, "
        "bounds: {a: [['', {}, true, false], [/1/, /1/, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:/2/},{a:/0/},{a:/1/}]}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null, "
        "bounds: {a: [['', {}, true, false], [/2/, /2/, true, true]]}}}}}");
}

//
// Negation
//

TEST_F(QueryPlannerTest, NegationIndexForSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: 1}}"), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1}, "
        "bounds: {a: [['MinKey',1,true,false], "
        "[1,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegationTopLevel) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: 1}}"), BSONObj(), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',1,true,false], "
        "[1,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegationOr) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NegationOrNotIn) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {b: {$nin: [1]}}]}"), BSONObj(), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NegationAndIndexOnEquality) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$and: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegationAndIndexOnEqualityAndNegationBranches) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortProj(fromjson("{$and: [{a: 1}, {b: 2}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1}, "
        "bounds: {a: [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {b: 1}, "
        "bounds: {b: [[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegationAndIndexOnInequality) {
    addIndex(BSON("b" << 1));
    runQuerySortProj(fromjson("{$and: [{a: 1}, {b: {$ne: 1}}]}"), BSONObj(), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: {ixscan: {pattern: {b:1}, "
        "bounds: {b: [['MinKey',1,true,false], "
        "[1,'MaxKey',false,true]]}}}}}");
}

// Negated regexes don't use the index.
TEST_F(QueryPlannerTest, NegationRegexPrefix) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: /^a/}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Negated mods don't use the index
TEST_F(QueryPlannerTest, NegationMod) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$mod: [2, 1]}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Negated $type doesn't use the index
TEST_F(QueryPlannerTest, NegationTypeOperator) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$type: 16}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Negated $elemMatch value won't use the index
TEST_F(QueryPlannerTest, NegationElemMatchValue) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {$gt: 3, $lt: 10}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Negated $elemMatch object won't use the index
TEST_F(QueryPlannerTest, NegationElemMatchObject) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {j: 1}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Negated $elemMatch object won't use the index
TEST_F(QueryPlannerTest, NegationElemMatchObject2) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {j: {$ne: 1}}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// If there is a negation that can't use the index,
// ANDed with a predicate that can use the index, then
// we can still use the index for the latter predicate.
TEST_F(QueryPlannerTest, NegationRegexWithIndexablePred) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{$and: [{i: {$not: /o/}}, {i: 2}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegationCantUseSparseIndex) {
    // false means not multikey, true means sparse
    addIndex(BSON("i" << 1), false, true);
    runQuery(fromjson("{i: {$ne: 4}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NegationCantUseSparseIndex2) {
    // false means not multikey, true means sparse
    addIndex(BSON("i" << 1 << "j" << 1), false, true);
    runQuery(fromjson("{i: 4, j: {$ne: 5}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {i:1,j:1}, bounds: "
        "{i: [[4,4,true,true]], j: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegatedRangeStrGT) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$gt: 'a'}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [['MinKey','a',true,true], "
        "[{},'MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegatedRangeStrGTE) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$gte: 'a'}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [['MinKey','a',true,false], "
        "[{},'MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegatedRangeIntGT) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$gt: 5}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [['MinKey',5,true,true], "
        "[Infinity,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NegatedRangeIntGTE) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$gte: 5}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [['MinKey',5,true,false], "
        "[Infinity,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, TwoNegatedRanges) {
    addIndex(BSON("i" << 1));
    runQuery(
        fromjson("{$and: [{i: {$not: {$lte: 'b'}}}, "
                 "{i: {$not: {$gte: 'f'}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {i:1}, "
        "bounds: {i: [['MinKey','',true,false], "
        "['b','f',false,false], "
        "[{},'MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, AndWithNestedNE) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$gt: -1, $lt: 1, $ne: 0}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [[-1,0,false,false], "
        "[0,1,false,false]]}}}}}");
}

TEST_F(QueryPlannerTest, NegatePredOnCompoundIndex) {
    addIndex(BSON("x" << 1 << "a" << 1));
    runQuery(fromjson("{x: 1, a: {$ne: 1}, b: {$ne: 2}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {x:1,a:1}, bounds: "
        "{x: [[1,1,true,true]], "
        "a: [['MinKey',1,true,false], [1,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NEOnMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$ne: 3}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$ne:3}}, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',3,true,false],"
        "[3,'MaxKey',false,true]]}}}}}");
}

// In general, a negated $nin can make use of an index.
TEST_F(QueryPlannerTest, NinUsesMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$nin: [4, 10]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$nin:[4,10]}}, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',4,true,false],"
        "[4,10,false,false],"
        "[10,'MaxKey',false,true]]}}}}}");
}

// But it can't if the $nin contains a regex because regex bounds can't
// be complemented.
TEST_F(QueryPlannerTest, NinCantUseMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$nin: [4, /foobar/]}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

//
// Multikey indices
//

//
// Index bounds related tests
//

TEST_F(QueryPlannerTest, CompoundIndexBoundsLastFieldMissing) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuery(fromjson("{a: 5, b: {$gt: 7}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: "
        "{a: [[5,5,true,true]], b: [[7,Infinity,false,true]], "
        " c: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsMiddleFieldMissing) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuery(fromjson("{a: 1, c: {$lt: 3}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]], "
        " c: [[-Infinity,3,true,false]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsRangeAndEquality) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: {$gt: 8}, b: 6}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[8,Infinity,false,true]], b:[[6,6,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsEqualityThenIn) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: 5, b: {$in: [2,6,11]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
        "{a: 1, b: 1}, bounds: {a: [[5,5,true,true]], "
        "b:[[2,2,true,true],[6,6,true,true],[11,11,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsStringBounds) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: {$gt: 'foo'}, b: {$gte: 'bar'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
        "{a: 1, b: 1}, bounds: {a: [['foo',{},false,false]], "
        "b:[['bar',{},true,false]]}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsAndWithNestedOr) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$and: [{a: 1, $or: [{a: 2}, {a: 3}]}]}"));

    // Given that the index over 'a' isn't multikey, we ideally won't generate any solutions
    // since we know the query describes an empty set if 'a' isn't multikey.  Any solutions
    // below are "this is how it currently works" instead of "this is how it should work."

    // It's kind of iffy to look for indexed solutions so we don't...
    size_t matches = 0;
    matches += numSolutionMatches(
        "{cscan: {dir: 1, filter: "
        "{$or: [{a: 2, a:1}, {a: 3, a:1}]}}}");
    matches += numSolutionMatches(
        "{cscan: {dir: 1, filter: "
        "{$and: [{$or: [{a: 2}, {a: 3}]}, {a: 1}]}}}");
    ASSERT_GREATER_THAN_OR_EQUALS(matches, 1U);
}

TEST_F(QueryPlannerTest, IndexBoundsIndexedSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("a" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a:1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {filter: {$or:[{a:1},{a:2}]}, dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsUnindexedSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {filter: {$or:[{a:1},{a:2}]}, dir: 1}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: {fetch: "
        "{filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsUnindexedSortHint) {
    addIndex(BSON("a" << 1));
    runQuerySortHint(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: {fetch: "
        "{filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsIntersectRanges) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));
    runQuery(fromjson("{a: {$gt: 1, $lt: 10}, c: {$gt: 1, $lt: 10}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1,b:1,c:1}, "
        "bounds: {a: [[1,10,false,false]], "
        "b: [['MinKey','MaxKey',true,true]], "
        "c: [[1,10,false,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1,c:1}, "
        "bounds: {a: [[1,10,false,false]], "
        "c: [[1,10,false,false]]}}}}}");
}

// Test that planner properly unionizes the index bounds for two negation
// predicates (SERVER-13890).
TEST_F(QueryPlannerTest, IndexBoundsOrOfNegations) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a: {$ne: 3}}, {a: {$ne: 4}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, BoundsTypeMinKeyMaxKey) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{a: {$type: -1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1}, bounds:"
        "{a: [['MinKey','MinKey',true,true]]}}}}}");

    runQuery(fromjson("{a: {$type: 127}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1}, bounds:"
        "{a: [['MaxKey','MaxKey',true,true]]}}}}}");
}

//
// Tests related to building index bounds for multikey
// indices, combined with compound and $elemMatch
//

// SERVER-12475: make sure that we compound bounds, even
// for a multikey index.
TEST_F(QueryPlannerTest, CompoundMultikeyBounds) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b" << 1), true);
    runQuery(fromjson("{a: 1, b: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {filter: {$and:[{a:1},{b:3}]}, dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1,b:1}, bounds: "
        "{a: [[1,1,true,true]], b: [[3,3,true,true]]}}}}}");
}

// Make sure that we compound bounds but do not intersect bounds
// for a compound multikey index.
TEST_F(QueryPlannerTest, CompoundMultikeyBoundsNoIntersect) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b" << 1), true);
    runQuery(fromjson("{a: 1, b: {$gt: 3, $lte: 5}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt:3}}, node: {ixscan: {filter: null, "
        "pattern: {a:1,b:1}, bounds: "
        "{a: [[1,1,true,true]], b: [[-Infinity,5,true,true]]}}}}}");
}

//
// QueryPlannerParams option tests
//

TEST_F(QueryPlannerTest, NoBlockingSortsAllowedTest) {
    params.options = QueryPlannerParams::NO_BLOCKING_SORT;
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());
    assertNumSolutions(0U);

    addIndex(BSON("x" << 1));

    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, NoTableScanBasic) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    runQuery(BSONObj());
    assertNumSolutions(0U);

    addIndex(BSON("x" << 1));

    runQuery(BSONObj());
    assertNumSolutions(0U);

    runQuery(fromjson("{x: {$gte: 0}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, NoTableScanOrWithAndChild) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1}}}, "
        "{fetch: {filter: {b: 7}, node: {ixscan: "
        "{filter: null, pattern: {a: 1}}}}}]}}}}");
}

//
// Index Intersection.
//
// We don't exhaustively check all plans here.  Instead we check that there exists an
// intersection plan.  The blending of >1 index plans and ==1 index plans is under development
// but we want to make sure that we create an >1 index plan when we should.
//

TEST_F(QueryPlannerTest, IntersectBasicTwoPred) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{a:1, b:{$gt: 1}}"));

    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompound) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{a:1, b:1, c:1}"));

    // There's an andSorted not andHash because the two seeks are point intervals.
    assertSolutionExists(
        "{fetch: {filter: null, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1, c:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

// SERVER-12196
TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompoundMatchesIdxOrder1) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{a:1, b:1}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {b:1}, node: "
        "{ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

// SERVER-12196
TEST_F(QueryPlannerTest, IntersectBasicTwoPredCompoundMatchesIdxOrder2) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("b" << 1));
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a:1, b:1}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {b:1}, node: "
        "{ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, IntersectManySelfIntersections) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    // True means multikey.
    addIndex(BSON("a" << 1), true);

    // This one goes to 11.
    runQuery(fromjson("{a:1, a:2, a:3, a:4, a:5, a:6, a:7, a:8, a:9, a:10, a:11}"));

    // But this one only goes to 10.
    assertSolutionExists(
        "{fetch: {filter: {a:11}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 1
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 2
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 3
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 4
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 5
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 6
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 7
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 8
        "{ixscan: {filter: null, pattern: {a:1}}},"        // 9
        "{ixscan: {filter: null, pattern: {a:1}}}]}}}}");  // 10
}

TEST_F(QueryPlannerTest, IntersectSubtreeNodes) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$or: [{a: 1}, {b: 1}], $or: [{c:1}, {d:1}]}"));
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: [{ixscan:{filter:null, pattern:{a:1}}},"
        "{ixscan:{filter:null, pattern:{b:1}}}]}},"
        "{or: {nodes: [{ixscan:{filter:null, pattern:{c:1}}},"
        "{ixscan:{filter:null, pattern:{d:1}}}]}}]}}}}");
}

TEST_F(QueryPlannerTest, IntersectSubtreeAndPred) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    runQuery(fromjson("{a: 1, $or: [{b:1}, {c:1}]}"));

    // This (can be) rewritten to $or:[ {a:1, b:1}, {c:1, d:1}].  We don't look for the various
    // single $or solutions as that's tested elsewhere.  We look for the intersect solution,
    // where each AND inside of the root OR is an and_sorted.
    size_t matches = 0;
    matches += numSolutionMatches(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a':1}}},"
        "{ixscan: {filter: null, pattern: {'b':1}}}]}},"
        "{andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a':1}}},"
        "{ixscan: {filter: null, pattern: {'c':1}}}]}}]}}}}");
    matches += numSolutionMatches(
        "{fetch: {filter: null, node: {andHash: {nodes:["
        "{or: {nodes: [{ixscan:{filter:null, pattern:{b:1}}},"
        "{ixscan:{filter:null, pattern:{c:1}}}]}},"
        "{ixscan:{filter: null, pattern:{a:1}}}]}}}}");
    ASSERT_GREATER_THAN_OR_EQUALS(matches, 1U);
}

TEST_F(QueryPlannerTest, IntersectElemMatch) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a.b" << 1));
    addIndex(BSON("a.c" << 1));
    runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:1, c:1}}},"
        "node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a.b':1}}},"
        "{ixscan: {filter: null, pattern: {'a.c':1}}}]}}}}");
}

TEST_F(QueryPlannerTest, IntersectSortFromAndHash) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortProj(fromjson("{a: 1, b:{$gt: 1}}"), fromjson("{b:1}"), BSONObj());

    // This provides the sort.
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Rearrange the preds, shouldn't matter.
    runQuerySortProj(fromjson("{b: 1, a:{$lt: 7}}"), fromjson("{b:1}"), BSONObj());
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, IntersectCanBeVeryBig) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1));
    runQuery(
        fromjson("{$or: [{ 'a' : null, 'b' : 94, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 98, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 1, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 2, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 7, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 9, 'c' : null, 'd' : null },"
                 "{ 'a' : null, 'b' : 16, 'c' : null, 'd' : null }]}"));

    assertNumSolutions(internalQueryEnumerationMaxOrSolutions.load());
}

// Ensure that disabling AND_HASH intersection works properly.
TEST_F(QueryPlannerTest, IntersectDisableAndHash) {
    bool oldEnableHashIntersection = internalQueryPlannerEnableHashIntersection.load();

    // Turn index intersection on but disable hash-based intersection.
    internalQueryPlannerEnableHashIntersection.store(false);
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{a: {$gt: 1}, b: 1, c: 1}"));

    // We should do an AND_SORT intersection of {b: 1} and {c: 1}, but no AND_HASH plans.
    assertNumSolutions(4U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1, c: 1}, node: {ixscan: "
        "{pattern: {a: 1}, bounds: {a: [[1,Infinity,false,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:1},c:1}, node: {ixscan: "
        "{pattern: {b: 1}, bounds: {b: [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:1},b:1}, node: {ixscan: "
        "{pattern: {c: 1}, bounds: {c: [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:1}}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {b:1}}},"
        "{ixscan: {filter: null, pattern: {c:1}}}]}}}}");

    // Restore the old value of the has intersection switch.
    internalQueryPlannerEnableHashIntersection.store(oldEnableHashIntersection);
}

//
// Index intersection cases for SERVER-12825: make sure that
// we don't generate an ixisect plan if a compound index is
// available instead.
//

// SERVER-12825
TEST_F(QueryPlannerTest, IntersectCompoundInsteadBasic) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: 1, b: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {b:1}, node: "
        "{ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a:1,b:1}}}}}");
}

// SERVER-12825
TEST_F(QueryPlannerTest, IntersectCompoundInsteadThreeCompoundIndices) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "d" << 1));
    addIndex(BSON("a" << 1 << "c" << -1 << "b" << -1 << "d" << 1));
    runQuery(fromjson("{a: 1, b: 1, c: 1, d: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{c:1},{d:1}]}, node: "
        "{ixscan: {filter: null, pattern: {a:1,b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:1},{b:1}]}, node: "
        "{ixscan: {filter: null, pattern: {c:1,d:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a:1,c:-1,b:-1,d:1}}}}}");
}

// SERVER-12825
TEST_F(QueryPlannerTest, IntersectCompoundInsteadUnusedField) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuery(fromjson("{a: 1, b: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {b:1}, node: "
        "{ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a:1,b:1,c:1}}}}}");
}

// SERVER-12825
TEST_F(QueryPlannerTest, IntersectCompoundInsteadUnusedField2) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "d" << 1));
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQuery(fromjson("{a: 1, c: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {c:1}, node: "
        "{ixscan: {filter: null, pattern: {a:1,b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {c:1,d:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a:1,b:1,c:1}}}}}");
}

//
// Test that we add a KeepMutations when we should and and we don't add one when we shouldn't.
//

// Collection scan doesn't keep any state, so it can't produce flagged data.
TEST_F(QueryPlannerTest, NoMutationsForCollscan) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    runQuery(fromjson(""));
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Collscan + sort doesn't produce flagged data either.
TEST_F(QueryPlannerTest, NoMutationsForSort) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    runQuerySortProj(fromjson(""), fromjson("{a:1}"), BSONObj());
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
}

// A basic index scan, fetch, and sort plan cannot produce flagged data.
TEST_F(QueryPlannerTest, MutationsFromFetchWithSort) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: 5}"), fromjson("{b:1}"), BSONObj());
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a:1}}}}}}}}}");
}

// Index scan w/covering doesn't require a keep node.
TEST_F(QueryPlannerTest, NoFetchNoKeep) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    addIndex(BSON("x" << 1));
    // query, sort, proj
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

    // cscan is a soln but we override the params that say to include it.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {ixscan: "
        "{filter: null, pattern: {x: 1}}}}}");
}

// No keep with geoNear.
TEST_F(QueryPlannerTest, NoKeepWithGeoNear) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{geoNear2d: {a: '2d'}}");
}

// No keep when we have an indexed sort.
TEST_F(QueryPlannerTest, NoKeepWithIndexedSort) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: {$in: [1, 2]}}"), BSON("b" << 1), BSONObj(), 0, 1);

    // cscan solution exists but we didn't turn on the "always include a collscan."
    assertNumSolutions(1);
    assertSolutionExists(
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, b: 1}}}, {ixscan: {pattern: {a: 1, b: 1}}}]}}}}");
}

// No KeepMutations when we have a sort that is not root, like the ntoreturn hack.
TEST_F(QueryPlannerTest, NoKeepWithNToReturn) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    runQuerySortProjSkipNToReturn(fromjson("{a: 1}"), fromjson("{b: 1}"), BSONObj(), 0, 3);

    assertSolutionExists(
        "{ensureSorted: {pattern: {b: 1}, node: "
        "{or: {nodes: ["
        "{sort: {pattern: {b: 1}, limit: 3, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}, "
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}]}}}}");
}

// Mergesort plans do not require a keep mutations stage.
TEST_F(QueryPlannerTest, NoKeepWithMergeSort) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;

    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: {$in: [1, 2]}}"), BSON("b" << 1), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1},"
        "bounds: {a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1},"
        "bounds: {a: [[2,2,true,true]], b: [['MinKey','MaxKey',true,true]]}}}]}}}}");
}

// Hash-based index intersection plans require a keep mutations stage.
TEST_F(QueryPlannerTest, AndHashRequiresKeepMutations) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{a: {$gte: 0}, b: {$gte: 0}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{fetch: {filter: {a: {$gte: 0}}, node: {ixscan: {pattern: {b: 1}}}}}");
    assertSolutionExists("{fetch: {filter: {b: {$gte: 0}}, node: {ixscan: {pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {keep: {node: {andHash: {nodes: ["
        "{ixscan: {pattern: {a: 1}}},"
        "{ixscan: {pattern: {b: 1}}}]}}}}}}");
}

// Sort-based index intersection plans require a keep mutations stage.
TEST_F(QueryPlannerTest, AndSortedRequiresKeepMutations) {
    params.options = QueryPlannerParams::KEEP_MUTATIONS;
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{a: 2, b: 3}"));

    assertNumSolutions(3U);
    assertSolutionExists("{fetch: {filter: {a: 2}, node: {ixscan: {pattern: {b: 1}}}}}");
    assertSolutionExists("{fetch: {filter: {b: 3}, node: {ixscan: {pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {keep: {node: {andSorted: {nodes: ["
        "{ixscan: {pattern: {a: 1}}},"
        "{ixscan: {pattern: {b: 1}}}]}}}}}}");
}

// Make sure a top-level $or hits the limiting number
// of solutions that we are willing to consider.
TEST_F(QueryPlannerTest, OrEnumerationLimit) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    // 6 $or clauses, each with 2 indexed predicates
    // means 2^6 = 64 possibilities. We should hit the limit.
    runQuery(
        fromjson("{$or: [{a: 1, b: 1},"
                 "{a: 2, b: 2},"
                 "{a: 3, b: 3},"
                 "{a: 4, b: 4},"
                 "{a: 5, b: 5},"
                 "{a: 6, b: 6}]}"));

    assertNumSolutions(internalQueryEnumerationMaxOrSolutions.load());
}

TEST_F(QueryPlannerTest, OrEnumerationLimit2) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1));

    // 3 $or clauses, and a few other preds. Each $or clause can
    // generate up to the max number of allowed $or enumerations.
    runQuery(
        fromjson("{$or: [{a: 1, b: 1, c: 1, d: 1},"
                 "{a: 2, b: 2, c: 2, d: 2},"
                 "{a: 3, b: 3, c: 3, d: 3}]}"));

    assertNumSolutions(internalQueryEnumerationMaxOrSolutions.load());
}

// SERVER-13104: test that we properly enumerate all solutions for nested $or.
TEST_F(QueryPlannerTest, EnumerateNestedOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{d: 1, $or: [{a: 1, b: 1}, {c: 1}]}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}},"
        "{ixscan: {pattern: {c: 1}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{ixscan: {pattern: {c: 1}}}]}}}}");
}

// SERVER-13104: test that we properly enumerate all solutions for nested $or.
TEST_F(QueryPlannerTest, EnumerateNestedOr2) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1));
    addIndex(BSON("e" << 1));
    addIndex(BSON("f" << 1));

    runQuery(fromjson("{a: 1, b: 1, $or: [{c: 1, d: 1}, {e: 1, f: 1}]}"));

    assertNumSolutions(6U);

    // Four possibilities from indexing the $or.
    assertSolutionExists(
        "{fetch: {filter: {a: 1, b: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {d: 1}, node: {ixscan: {pattern: {c: 1}}}}},"
        "{fetch: {filter: {f: 1}, node: {ixscan: {pattern: {e: 1}}}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, b: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}},"
        "{fetch: {filter: {f: 1}, node: {ixscan: {pattern: {e: 1}}}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, b: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {d: 1}, node: {ixscan: {pattern: {c: 1}}}}},"
        "{fetch: {filter: {e: 1}, node: {ixscan: {pattern: {f: 1}}}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, b: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}},"
        "{fetch: {filter: {e: 1}, node: {ixscan: {pattern: {f: 1}}}}}"
        "]}}}}");

    // Two possibilties from outside the $or.
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {b: 1}}}}}");
}

//
// Test the "split limited sort stages" hack.
//

TEST_F(QueryPlannerTest, SplitLimitedSort) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuerySortProjSkipNToReturn(fromjson("{a: 1}"), fromjson("{b: 1}"), BSONObj(), 0, 3);

    assertNumSolutions(2U);
    // First solution has no blocking stage; no need to split.
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
    // Second solution has a blocking sort with a limit: it gets split and
    // joined with an OR stage.
    assertSolutionExists(
        "{ensureSorted: {pattern: {b: 1}, node: "
        "{or: {nodes: ["
        "{sort: {pattern: {b: 1}, limit: 3, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}, "
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}}}]}}}}");
}

// The same query run as a find command with a limit should not require the "split limited sort"
// hack.
TEST_F(QueryPlannerTest, NoSplitLimitedSortAsCommand) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 1}, sort: {b: 1}, limit: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{limit: {n: 3, node: {fetch: {filter: {a:1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 3, node: {sortKeyGen: {node: {fetch: {filter: null,"
        "node: {ixscan: {pattern: {a: 1}}}}}}}}}");
}

// Same query run as a find command with a batchSize rather than a limit should not require
// the "split limited sort" hack, and should not have any limit represented inside the plan.
TEST_F(QueryPlannerTest, NoSplitLimitedSortAsCommandBatchSize) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: 1}, sort: {b: 1}, batchSize: 3}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: {ixscan: "
        "{filter: null, pattern: {b: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node: {fetch: {filter: null,"
        "node: {ixscan: {pattern: {a: 1}}}}}}}}}");
}

//
// Test shard filter query planning
//

TEST_F(QueryPlannerTest, ShardFilterCollScan) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: "
        "{cscan: {dir: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicIndex) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sharding_filter: {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuery(fromjson("{a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterBasicProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, type: 'coveredIndex', node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterCompoundProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1 << "b" << 1);
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1 }, type: 'coveredIndex', node: "
        "{sharding_filter: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterNestedProjCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1 << "b.c" << 1);
    addIndex(BSON("a" << 1 << "b.c" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id: 0, a: 1, 'b.c': 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, 'b.c': 1 }, type: 'default', node: "
        "{sharding_filter: {node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterHashProjNotCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a"
                           << "hashed");
    addIndex(BSON("a"
                  << "hashed"));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0,a: 1}, type: 'simple', node: "
        "{sharding_filter : {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {a: 'hashed'}}}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterKeyPrefixIndexCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a" << 1);
    addIndex(BSON("a" << 1 << "b" << 1 << "_id" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSONObj(), fromjson("{a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1}, type: 'coveredIndex', node: "
        "{sharding_filter : {node: "
        "{ixscan: {pattern: {a: 1, b: 1, _id: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, ShardFilterNoIndexNotCovered) {
    params.options = QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("a"
                           << "hashed");
    addIndex(BSON("b" << 1));

    runQuerySortProj(fromjson("{b: 1}"), BSONObj(), fromjson("{_id : 0, a : 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0,a: 1}, type: 'simple', node: "
        "{sharding_filter : {node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParam) {
    params.options = QueryPlannerParams::CANNOT_TRIM_IXISECT;
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{a: 1, b: 1, c: 1}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1, c: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1, c: 1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:1,b:1,c:1}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParamBeneathOr) {
    params.options = QueryPlannerParams::CANNOT_TRIM_IXISECT;
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{d: 1, $or: [{a: 1}, {b: 1, c: 1}]}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {c: 1}, node: {ixscan: {filter: null,"
        "pattern: {b: 1}, bounds: {b: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {filter: null,"
        "pattern: {c: 1}, bounds: {c: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {d: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1, c: 1}, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {b: 1}}},"
        "{ixscan: {filter: null, pattern: {c: 1}}}]}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectAndHashWithOrChild) {
    params.options = QueryPlannerParams::CANNOT_TRIM_IXISECT;
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{c: 1, $or: [{a: 1}, {b: 1, d: 1}]}"));

    assertNumSolutions(3U);

    assertSolutionExists(
        "{fetch: {filter: {c: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {d: 1}, node: {ixscan: {filter: null,"
        "pattern: {b: 1}, bounds: {b: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [[1,1,true,true]]}}}]}}}}");

    assertSolutionExists(
        "{fetch: {filter: {$or:[{b:1,d:1},{a:1}]}, node:"
        "{ixscan: {filter: null, pattern: {c: 1}}}}}");

    assertSolutionExists(
        "{fetch: {filter: {c:1,$or:[{a:1},{b:1,d:1}]}, node:{andHash:{nodes:["
        "{or: {nodes: ["
        "{fetch: {filter: {d:1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{ixscan: {filter: null, pattern: {a: 1}}}]}},"
        "{ixscan: {filter: null, pattern: {c: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, CannotTrimIxisectParamSelfIntersection) {
    params.options = QueryPlannerParams::CANNOT_TRIM_IXISECT;
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    // true means multikey
    addIndex(BSON("a" << 1), true);

    runQuery(fromjson("{a: {$all: [1, 2, 3]}}"));

    assertNumSolutions(4U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:2}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:1}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:2}, {a:3}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[1,1,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[2,2,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {a:1},"
        "bounds: {a: [[3,3,true,true]]}}}]}}}}");
}


// If a lookup against a unique index is available as a possible plan, then the planner
// should not generate other possibilities.
TEST_F(QueryPlannerTest, UniqueIndexLookup) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;
    params.options |= QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{a: 1, b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: "
        "{ixscan: {filter: null, pattern: {b: 1}}}}}");
}

TEST_F(QueryPlannerTest, HintOnNonUniqueIndex) {
    params.options = QueryPlannerParams::INDEX_INTERSECTION;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQueryHint(fromjson("{a: 1, b: 1}"), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, UniqueIndexLookupBelowOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{$or: [{a: 1, b: 1}, {c: 1, d: 1}]}"));

    // Only two plans because we throw out plans for the right branch of the $or that do not
    // use equality over the unique index.
    assertNumSolutions(2U);
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}]}}");
}

TEST_F(QueryPlannerTest, UniqueIndexLookupBelowOrBelowAnd) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuery(fromjson("{e: 1, $or: [{a: 1, b: 1}, {c: 1, d: 1}]}"));

    // Only two plans because we throw out plans for the right branch of the $or that do not
    // use equality over the unique index.
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {e: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {b: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {e: 1}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}},"
        "{fetch: {filter: {c: 1}, node: {ixscan: {pattern: {d: 1}}}}}"
        "]}}}}");
}

TEST_F(QueryPlannerTest, CoveredOrUniqueIndexLookup) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1),
             false,  // multikey
             false,  // sparse,
             true);  // unique

    runQuerySortProj(fromjson("{a: 1, b: 1}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {a: 1}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, SortKeyMetaProjection) {
    addIndex(BSON("a" << 1));

    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), fromjson("{b: {$meta: 'sortKey'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {b: {$meta: 'sortKey'}}, node: "
        "{sort: {limit: 0, pattern: {a: 1}, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {b: {$meta: 'sortKey'}}, node: "
        "{sortKeyGen: {node: {fetch: {filter: null, node: "
        "{ixscan: {pattern: {a: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, SortKeyMetaProjectionCovered) {
    addIndex(BSON("a" << 1));

    runQuerySortProj(
        BSONObj(), fromjson("{a: 1}"), fromjson("{_id: 0, a: 1, b: {$meta: 'sortKey'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: {$meta: 'sortKey'}}, node: "
        "{sort: {limit: 0, pattern: {a: 1}, node: "
        "{sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: {$meta: 'sortKey'}}, node: "
        "{sortKeyGen: {node: "
        "{ixscan: {pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, FloatingPointInKeyPattern) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << -0.1));

    runQuerySortProj(fromjson("{a: {$gte: 3, $lte: 5}}"), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: -0.1}, "
        "bounds: {a: [[3, 5, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, KeyPatternOverflowsInt) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << -2147483649LL));

    runQuerySortProj(fromjson("{a: {$gte: 3, $lte: 5}}"), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: -2147483649}, "
        "bounds: {a: [[3, 5, true, true]]}}}}}");
}

//
// Test bad input to query planner helpers.
//

TEST_F(QueryPlannerTest, CacheDataFromTaggedTreeFailsOnBadInput) {
    PlanCacheIndexTree* indexTree;

    // Null match expression.
    std::vector<IndexEntry> relevantIndices;
    Status s = QueryPlanner::cacheDataFromTaggedTree(NULL, relevantIndices, &indexTree);
    ASSERT_NOT_OK(s);
    ASSERT(NULL == indexTree);

    // No relevant index matching the index tag.
    relevantIndices.push_back(IndexEntry(BSON("a" << 1)));

    auto qr = stdx::make_unique<QueryRequest>(NamespaceString("test.collection"));
    qr->setFilter(BSON("a" << 3));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        opCtx.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> scopedCq = std::move(statusWithCQ.getValue());
    scopedCq->root()->setTag(new IndexTag(1));

    s = QueryPlanner::cacheDataFromTaggedTree(scopedCq->root(), relevantIndices, &indexTree);
    ASSERT_NOT_OK(s);
    ASSERT(NULL == indexTree);
}

TEST_F(QueryPlannerTest, TagAccordingToCacheFailsOnBadInput) {
    const NamespaceString nss("test.collection");

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(BSON("a" << 3));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        opCtx.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> scopedCq = std::move(statusWithCQ.getValue());

    std::unique_ptr<PlanCacheIndexTree> indexTree(new PlanCacheIndexTree());
    indexTree->setIndexEntry(IndexEntry(BSON("a" << 1), "a_1"));

    std::map<StringData, size_t> indexMap;

    // Null filter.
    Status s = QueryPlanner::tagAccordingToCache(NULL, indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);

    // Null indexTree.
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), NULL, indexMap);
    ASSERT_NOT_OK(s);

    // Index not found.
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);

    // Index found once added to the map.
    indexMap["a_1"_sd] = 0;
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_OK(s);

    // Regenerate canonical query in order to clear tags.
    auto newQR = stdx::make_unique<QueryRequest>(nss);
    newQR->setFilter(BSON("a" << 3));
    statusWithCQ = CanonicalQuery::canonicalize(
        opCtx.get(), std::move(newQR), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    scopedCq = std::move(statusWithCQ.getValue());

    // Mismatched tree topology.
    PlanCacheIndexTree* child = new PlanCacheIndexTree();
    child->setIndexEntry(IndexEntry(BSON("a" << 1)));
    indexTree->children.push_back(child);
    s = QueryPlanner::tagAccordingToCache(scopedCq->root(), indexTree.get(), indexMap);
    ASSERT_NOT_OK(s);
}

// A query run as a find command with a sort and ntoreturn should generate a plan implementing
// the 'ntoreturn hack'.
TEST_F(QueryPlannerTest, NToReturnHackWithFindCommand) {
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;

    runQueryAsCommand(fromjson("{find: 'testns', sort: {a:1}, ntoreturn:3}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{ensureSorted: {pattern: {a: 1}, node: "
        "{or: {nodes: ["
        "{sort: {limit:3, pattern: {a:1}, node: {sortKeyGen: {node: {cscan: {dir:1}}}}}}, "
        "{sort: {limit:0, pattern: {a:1}, node: {sortKeyGen: {node: {cscan: {dir:1}}}}}}"
        "]}}}}");
}

TEST_F(QueryPlannerTest, NToReturnHackWithSingleBatch) {
    params.options |= QueryPlannerParams::SPLIT_LIMITED_SORT;

    runQueryAsCommand(fromjson("{find: 'testns', sort: {a:1}, ntoreturn:3, singleBatch:true}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a:1}, limit:3, node: {sortKeyGen: {node: "
        "{cscan: {dir:1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, NorWithSingleChildCanUseIndexAfterComplementingBounds) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$nor: [{a: {$lt: 3}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1}, bounds:"
        "{a: [['MinKey', -Infinity, true, false], [3, 'MaxKey', true, true]]}}}}}");
}

// Multiple indexes
TEST_F(QueryPlannerTest, PlansForMultipleIndexesOnTheSameKeyPatternAreGenerated) {
    CollatorInterfaceMock reverseCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock equalCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(BSON("a" << 1), &reverseCollator, "reverse"_sd);
    addIndex(BSON("a" << 1), &equalCollator, "forward"_sd);

    runQuery(BSON("a" << 1));

    assertNumSolutions(3U);
    assertSolutionExists("{fetch: {node: {ixscan: {name: 'reverse'}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {name: 'forward'}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOr) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOneChildUsesPredicate) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrCombineWithAnd) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "d" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {$and: [{c: 7}, {d: 8}]}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, d: 1, a: 1}, bounds: {c: [[7, 7, true, true]], d: [[8, 8, true, "
        "true]], a: [[5, 5, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, NestedContainedOr) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("d" << 1 << "a" << 1));
    addIndex(BSON("e" << 1 << "a" << 1));

    runQuery(
        fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {$and: [{c: 7}, {$or: [{d: 8}, {e: 9}]}]}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{fetch: {filter: {c: 7}, node: {or: {nodes: ["
        "{ixscan: {pattern: {d: 1, a: 1}, bounds: {d: [[8, 8, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {e: 1, a: 1}, bounds: {e: [[9, 9, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, NestedContainedOrOneChildUsesPredicate) {
    addIndex(BSON("c" << 1 << "a" << 1));
    addIndex(BSON("d" << 1));
    addIndex(BSON("f" << 1));
    addIndex(BSON("g" << 1 << "a" << 1));

    runQuery(
        fromjson("{$and: [{a: 5}, {$or: [{$and: [{b: 6}, {$or: [{c: 7}, {d: 8}]}]}, "
                 "{$and: [{e: 9}, {$or: [{f: 10}, {g: 11}]}]}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{fetch: {filter: {b: 6}, node: {or: {nodes: ["
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[8, 8, true, true]]}}}"
        "]}}}},"
        "{fetch: {filter: {e: 9}, node: {or: {nodes: ["
        "{ixscan: {pattern: {f: 1}, bounds: {f: [[10, 10, true, true]]}}},"
        "{ixscan: {pattern: {g: 1, a: 1}, bounds: {g: [[11, 11, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, DoublyContainedOr) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));
    addIndex(BSON("d" << 1));

    runQuery(
        fromjson("{$and: [{$or: [{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}, {d: 8}]}, {e: 9}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {e: 9}, node: {or: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}]}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrNotNextInIndex) {
    addIndex(BSON("b" << 1 << "d" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, d: 1, a: 1}, bounds: {b: [[6, 6, true, true]], d: [['MinKey', "
        "'MaxKey', true, true]], a: [[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultiplePredicates) {
    addIndex(BSON("c" << 1 << "a" << 1 << "b" << 1));
    addIndex(BSON("d" << 1 << "b" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {b: 6}, {$or: [{c: 7}, {d: 8}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {c: 1, a: 1, b: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]], b: [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {d: 1, b: 1, a: 1}, bounds: {d: [[8, 8, true, true]], b: [[6, 6, true, "
        "true]], a: [[5, 5, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrIntersect) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(
        fromjson("{$and: [{a: {$gte: 5}}, {$or: [{b: 6}, {$and: [{c: 7}, {a: {$lte: 8}}]}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, Infinity, "
        "true, true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 8, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrNot) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{$nor: [{a: 5}]}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [['MinKey', 5, "
        "true, false], [5, 'MaxKey', false, true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [['MinKey', 5, "
        "true, false], [5, 'MaxKey', false, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveToNot) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{$nor: [{b: 6}]}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [['MinKey', 6, true, false], [6, 'MaxKey', "
        "false, true]], a: [[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldInIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [[7, 7, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldAndTrailingField) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldForOneOrBranch) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicatesAreLeadingFields) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    runQuery(fromjson("{$and: [{a: {$gte: 0}}, {a: {$lte: 10}}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[0, 10, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[0, 10, true, true]], c: [[7, 7, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[0, 10, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[0, 10, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOnePredicateIsLeadingField) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("a" << 1 << "b" << 1 << "d" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {b: 6}, {$or: [{c: 7}, {d: 8}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], c: [[7, 7, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1, d: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], d: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{c: 7}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], c: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{c: 7}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, d: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], d: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrCombineLeadingFields) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: {$gte: 0}}, {$or: [{a: {$lte: 10}}, {b: 6}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[0, 10, true, true]]}}},"
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[0, Infinity, "
        "true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: {$lte: 10}}, {b: 6}]}, node: "
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldMoveToAnd) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("a" << 1 << "d" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], c: [[7, 7, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[5, 5, true, true]], d: [[8, 8, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]], c: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[5, 5, true, true]], d: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldMoveToAndWithFilter) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "d" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {c: 7}, node: {ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, "
        "true]], b: [[6, 6, true, "
        "true]]}}}}},"
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[5, 5, true, true]], d: [[8, 8, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[5, 5, true, true]], d: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicatesAreLeadingFieldsMoveToAnd) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    addIndex(BSON("a" << 1 << "d" << 1));

    runQuery(fromjson(
        "{$and: [{a: {$gte: 0}}, {a: {$lte: 10}}, {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[0, 10, true, true]], b: [[6, 6, "
        "true, true]], c: [[7, 7, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[0, 10, true, true]], d: [[8, 8, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, bounds: {a: [[0, 10, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]], c: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}, node: "
        "{ixscan: {pattern: {a: 1, d: 1}, bounds: {a: [[0, 10, true, true]], d: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrOnePredicateIsLeadingFieldMoveToAnd) {
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));
    addIndex(BSON("a" << 1 << "b" << 1 << "e" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {b: 6}, {$or: [{$and: [{c: 7}, {d: 8}]}, {e: 9}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, "
        "true, true]], c: [[7, 7, true, true]], d: [[8, 8, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1, e: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], e: [[9, 9, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{c: 7}, {d: 8}]}, {e: 9}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, "
        "true, true]], c: [['MinKey', 'MaxKey', true, true]], d: [['MinKey', 'MaxKey', true, "
        "true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{c: 7}, {d: 8}]}, {e: 9}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, e: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]], e: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrCombineLeadingFieldsMoveToAnd) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(
        fromjson("{$and: [{a: {$gte: 0}}, {$or: [{$and: [{a: {$lte: 10}}, {b: 6}]}, {c: 7}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[0, 10, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[0, Infinity, "
        "true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{$and: [{a: {$lte: 10}}, {b: 6}]}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[0, Infinity, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldIndexIntersection) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(4);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPredicateIsLeadingFieldInBothBranchesIndexIntersection) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(6);
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [[7, 7, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    // The AND_HASH stage is not really needed, since the predicate {a: 5} is covered by the indexed
    // OR.
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [[7, 7, true, "
        "true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [[7, 7, true, "
        "true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrNotPredicateIsLeadingFieldIndexIntersection) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{$nor: [{a: 5}]}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(4);
    // The filter is {$not: {a: 5}}, but there is no way to write a BSON expression that will parse
    // to that MatchExpression.
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrNotPredicateIsLeadingFieldInBothBranchesIndexIntersection) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    runQuery(fromjson("{$and: [{$nor: [{a: 5}]}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(6);
    // The filter is {$not: {a: 5}}, but there is no way to write a BSON expression that will parse
    // to that MatchExpression.
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}]}, node: "
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    // The AND_HASH stage is not really needed, since the predicate {a: 5} is covered by the indexed
    // OR.
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCombineLeadingFields) {
    const bool multikey = true;
    addIndex(BSON("a" << 1), multikey);
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{$and: [{a: {$gte: 0}}, {$or: [{a: {$lte: 10}}, {b: 6}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {b: 1}, bounds: {b: [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: {$lte: 10}}, {b: 6}]}, node: "
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCombineLeadingFields) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("a" << 1), multikeyPaths);
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{$and: [{a: {$gte: 0}}, {$or: [{a: {$lte: 10}}, {b: 6}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {b: 1}, bounds: {b: [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: {$lte: 10}}, {b: 6}]}, node: "
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCombineLeadingFields) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "c" << 1), multikeyPaths);
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{$and: [{a: {$gte: 0}}, {$or: [{a: {$lte: 10}}, {b: 6}]}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[0, 10, true, true]]}}},"
        "{ixscan: {pattern: {b: 1}, bounds: {b: [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: {$lte: 10}}, {b: 6}]}, node: "
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[0, Infinity, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCompoundFields) {
    const bool multikey = true;
    addIndex(BSON("b" << 1 << "a" << 1), multikey);
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCompoundFields) {
    const bool multikey = true;
    addIndex(BSON("a.c" << 1 << "a.b" << 1), multikey);
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$and: [{'a.b': 5}, {$or: [{'a.c': 6}, {d: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCompoundFields) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("b" << 1 << "a" << 1), multikeyPaths);
    addIndex(BSON("c" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCompoundDottedFields) {
    MultikeyPaths multikeyPaths{{1U}, {1U}};
    addIndex(BSON("a.c" << 1 << "a.b" << 1), multikeyPaths);
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$and: [{'a.b': 5}, {$or: [{'a.c': 6}, {d: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCompoundFields) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.c" << 1 << "a.b" << 1), multikeyPaths);
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$and: [{'a.b': 5}, {$or: [{'a.c': 6}, {d: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCombineTrailingFields) {
    const bool multikey = true;
    addIndex(BSON("b" << 1 << "a" << 1), multikey);
    addIndex(BSON("c" << 1));

    runQuery(
        fromjson("{$and: [{a: {$gte: 0}}, {$or: [{$and: [{a: {$lte: 10}}, {b: 6}]}, {c: 7}]}]}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[-Infinity, 10, "
        "true, true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[0, Infinity, "
        "true, true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCombineTrailingFields) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("b" << 1 << "a" << 1), multikeyPaths);
    addIndex(BSON("c" << 1));

    runQuery(
        fromjson("{$and: [{a: {$gte: 0}}, {$or: [{$and: [{a: {$lte: 10}}, {b: 6}]}, {c: 7}]}]}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[-Infinity, 10, "
        "true, true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[0, Infinity, "
        "true, true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCombineTrailingFields) {
    MultikeyPaths multikeyPaths{{0U}, {}};
    addIndex(BSON("b" << 1 << "a" << 1), multikeyPaths);
    addIndex(BSON("c" << 1));

    runQuery(
        fromjson("{$and: [{a: {$gte: 0}}, {$or: [{$and: [{a: {$lte: 10}}, {b: 6}]}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 0}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[0, 10, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCompoundTrailingFields) {
    const bool multikey = true;
    addIndex(BSON("b" << 1 << "a" << 1 << "c" << 1), multikey);
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1, c: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]], c: [[7, 7, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCompoundTrailingFields) {
    const bool multikey = true;
    addIndex(BSON("d" << 1 << "a.b" << 1 << "a.c" << 1), multikey);
    addIndex(BSON("e" << 1));

    runQuery(fromjson("{$and: [{'a.b': 5}, {$or: [{$and: [{'a.c': 6}, {d: 7}]}, {e: 8}]}]}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.b': 1, 'a.c': 1}, bounds: {d: [[7, 7, true, true]], 'a.b': "
        "[['MinKey', 'MaxKey', true, true]], 'a.c': [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.b': 1, 'a.c': 1}, bounds: {d: [[7, 7, true, true]], 'a.b': "
        "[[5, 5, true, true]], 'a.c': [['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCompoundTrailingFields) {
    MultikeyPaths multikeyPaths{{}, {0U}, {}};
    addIndex(BSON("b" << 1 << "a" << 1 << "c" << 1), multikeyPaths);
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{$and: [{b: 6}, {c: 7}]}, {d: 8}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: 5}, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1, c: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]], c: [[7, 7, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCompoundTrailingFields) {
    MultikeyPaths multikeyPaths{{}, {0U}, {0U}};
    addIndex(BSON("d" << 1 << "a.b" << 1 << "a.c" << 1), multikeyPaths);
    addIndex(BSON("e" << 1));

    runQuery(fromjson("{$and: [{'a.b': 5}, {$or: [{$and: [{'a.c': 6}, {d: 7}]}, {e: 8}]}]}"));
    assertNumSolutions(2);
    // When we have path-level multikey info, we ensure that predicates are assigned in order of
    // index position.
    assertSolutionExists(
        "{fetch: {filter: {'a.b': 5}, node: {or: {nodes: ["
        "{fetch: {filter: {'a.c': 6}, node: {ixscan: {pattern: {d: 1, 'a.b': 1, 'a.c': 1}, bounds: "
        "{d: [[7, 7, true, true]], 'a.b': [[5, 5, true, true]], 'a.c': [['MinKey', 'MaxKey', true, "
        "true]]}}}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithoutProjectionUsesCollscan) {
    addIndex(BSON("a" << 1));
    runQuery(BSONObj());
    assertNumSolutions(1);
    assertSolutionExists("{cscan: {dir: 1}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCoveredIxscanIfEnabled) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1},"
        "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionDoesNotUseCoveredIxscanIfDisabled) {
    params.options &= ~QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCoveredIxscanOnCompoundIndexIfEnabled) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1, c: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, c: 1}, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1, c: 1}, bounds:"
        "{a: [['MinKey', 'MaxKey', true, true]], b: [['MinKey', 'MaxKey', true, true]],"
        "c: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionDoesNotUseCoveredIxscanOnCompoundIndexIfDisabled) {
    params.options &= ~QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1, c: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, c: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionDoesNotConsiderNonHintedIndices) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}, hint: {_id: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {_id: 1}, "
        "bounds: {_id: [['MinKey', 'MaxKey', true, true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfNoCoveredIxscans) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {a: 1}, node:"
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest,
       EmptyQueryWithProjectionUsesCoveredIxscanOnDotttedNonMultikeyIndexIfEnabled) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a.b" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, 'a.b': 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1},"
        "bounds: {'a.b': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest,
       EmptyQueryWithProjectionDoesNotUseCoveredIxscanOnDotttedNonMultikeyIndexIfDisabled) {
    params.options &= ~QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a.b" << 1));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, 'a.b': 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexIsMultikey) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    constexpr bool isMultikey = true;
    addIndex(BSON("a" << 1 << "b" << 1), isMultikey);
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1, b: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexIsSparse) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    constexpr bool isMultikey = false;
    constexpr bool isSparse = true;
    addIndex(BSON("a" << 1), isMultikey, isSparse);
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1, b: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexIsPartial) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    AlwaysFalseMatchExpression matchExpr;
    addIndex(BSON("a" << 1), &matchExpr);
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexIsText) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a"
                  << "text"));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexIsGeo) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    addIndex(BSON("a"
                  << "2dsphere"));
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerTest, EmptyQueryWithProjectionUsesCollscanIfIndexCollationDiffers) {
    params.options = QueryPlannerParams::GENERATE_COVERED_IXSCANS;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(BSON("a" << 1), &collator);
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}}"));
    assertNumSolutions(1);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}");
}
}  // namespace
