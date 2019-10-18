/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
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
        "{fetch: {node: {sort: {pattern: {d: 1}, limit: 1, node: {sortKeyGen: {node: {ixscan: "
        "{pattern: {a: 1, b: 1, c:1, d:1}}}}}}}}}");
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
        "{fetch: {node: {sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
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
        "{fetch: {node: {sort: {pattern: {b:1}, limit: 0, node: {sortKeyGen: {node: "
        "{ixscan: {pattern: {a:1,b:1,c:1}}}}}}}}}");
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
        "{fetch: {filter: null, node: {sort: {pattern: {c: 1}, "
        "limit: 0, node: {sortKeyGen: {node: {or: {nodes: ["
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

TEST_F(QueryPlannerTest, ContainedOrNotEqualsNull) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{$nor: [{a: null}]}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [['MinKey', "
        "undefined, true, false], [null, 'MaxKey', false, true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [['MinKey', "
        "undefined, true, false], [null, 'MaxKey', false, true]]}}}"
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
        "{fetch: {filter: {$and:[{a:5},{$or:[{a:5,b:6},{c:7}]}]}, node: {andHash: {nodes: ["
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
        "{fetch: {filter: {$and:[{a:5},{$or:[{a:5,b:6},{a:5,c:7}]}]}, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[5, 5, true, true]], c: [[7, 7, true, "
        "true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:5},{$or:[{a:5,b:6},{a:5,c:7}]}]}, node: {andHash: {nodes: ["
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
        "{fetch: {filter: {$and:[{a: {$ne: 5}},{$or:[{a:{$ne:5},b:6},{c:7}]}]},"
        "node: {andHash: {nodes: ["
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
        "{fetch: {filter: {$and:[{a: {$ne: 5}},{$or:[{a:{$ne:5},b:6},{a:{$ne:5},c:7}]}]},"
        "node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [['MinKey', 5, true, false], [5, 'MaxKey', "
        "false, true]], b: [['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a: {$ne: 5}},{$or:[{a:{$ne:5},b:6},{a:{$ne:5},c:7}]}]},"
        "node: {andHash: {nodes: ["
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

TEST_F(QueryPlannerTest, MultipleContainedOrWithIndexIntersectionEnabled) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1 << "a" << 1));
    addIndex(BSON("e" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}, {$or: [{d: 8}, {e: 9}]}]}"));

    assertNumSolutions(6U);

    // Non-ixisect solutions.
    assertSolutionExists(
        "{fetch: {filter: {$or: [{d: 8}, {e: 9}], a: 5}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {b: 1, a: 1},"
        "bounds: {b: [[6,6,true,true]], a: [[5,5,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {c: 1}, bounds: {c: [[7,7,true,true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{b: 6}, {c: 7}], a: 5}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {d: 1, a: 1},"
        "bounds: {d: [[8,8,true,true]], a: [[5,5,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {e: 1}, bounds: {e: [[9,9,true,true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{$or: [{b: 6}, {c: 7}]}, {$or: [{d: 8}, {e: 9}]}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}, bounds: {a: [[5,5,true,true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");

    // Ixisect solutions.
    assertSolutionExists(
        "{fetch: {node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {filter: null, pattern: {b: 1, a: 1},"
        "bounds: {b: [[6,6,true,true]], a: [[5,5,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {c: 1}, bounds: {c: [[7,7,true,true]]}}}"
        "]}},"
        "{ixscan: {filter: null, pattern: {a: 1}, bounds: {a: [[5,5,true,true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {filter: null, pattern: {d: 1, a: 1},"
        "bounds: {d: [[8,8,true,true]], a: [[5,5,true,true]]}}},"
        "{ixscan: {filter: null, pattern: {e: 1}, bounds: {e: [[9,9,true,true]]}}}"
        "]}},"
        "{ixscan: {filter: null, pattern: {a: 1}, bounds: {a: [[5,5,true,true]]}}}"
        "]}}}}");
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
        "{ixscan: {pattern: {a: 1, c: 1}, bounds: {a: [[0, 10, true, true]], c: [['MinKey', "
        "'MaxKey', true, true]]}}},"
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

TEST_F(QueryPlannerTest, ContainedOrPushdownIndexedExpr) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuery(
        fromjson("{$expr: {$and: [{$eq: ['$d', 'd']}, {$eq: ['$a', 'a']}]},"
                 "$or: [{b: 'b'}, {b: 'c'}]}"));
    assertNumSolutions(3);
    // When we have path-level multikey info, we ensure that predicates are assigned in order of
    // index position.
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null, bounds: {a: [['a', 'a', true, true]], b: "
        "[['b', 'b', true, true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null, bounds: {a: [['a', 'a', true, true]], b: "
        "[['c', 'c', true, true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, filter: null,"
        "bounds: {a: [['a', 'a', true, true]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}}}");
}

// SERVER-41872 fixed a case where variable "choice" ordering in the PlanEnumerator memo could lead
// to different sets of solutions generated for the same input. This would occur in the case where
// we only enumerate a subset of possible plans due to reaching internal limits and enumerate plans
// in a non-stable order. With the fix for SERVER-41872, PlanEnumerator ordering is stable and
// expected to always return the same set of solutions for a given input.
TEST_F(QueryPlannerTest, SolutionSetStableWhenOrEnumerationLimitIsReached) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("d" << 1));
    addIndex(BSON("e" << 1));
    addIndex(BSON("f" << 1));
    addIndex(BSON("f" << 1 << "y" << 1));
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("c" << 1 << "x" << 1));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 1, b: 1, c: 1}, {d: 1, e: 1, f: 1}]}}"));

    assertNumSolutions(10U);

    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {b: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {a: "
        "1}}}}}, {fetch: {filter: {e: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {d: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {b: "
        "1}}}}}, {fetch: {filter: {e: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {d: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, b: {$eq: 1} }, node: {ixscan: {pattern: {c: "
        "1}}}}}, {fetch: {filter: {e: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {d: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, b: {$eq: 1} }, node: {ixscan: {pattern: {c: "
        "1, x: 1}}}}}, {fetch: {filter: {e: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {d: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {b: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {a: "
        "1}}}}}, {fetch: {filter: {d: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {e: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {b: "
        "1}}}}}, {fetch: {filter: {d: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {e: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, b: {$eq: 1} }, node: {ixscan: {pattern: {c: "
        "1}}}}}, {fetch: {filter: {d: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {e: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, b: {$eq: 1} }, node: {ixscan: {pattern: {c: "
        "1, x: 1}}}}}, {fetch: {filter: {d: {$eq: 1}, f: {$eq: 1} }, node: {ixscan: {pattern: {e: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {b: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {a: "
        "1}}}}}, {fetch: {filter: {d: {$eq: 1}, e: {$eq: 1} }, node: {ixscan: {pattern: {f: "
        "1}}}}}]}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {filter: {a: {$eq: 1}, c: {$eq: 1} }, node: {ixscan: {pattern: {b: "
        "1}}}}}, {fetch: {filter: {d: {$eq: 1}, e: {$eq: 1} }, node: {ixscan: {pattern: {f: "
        "1}}}}}]}}");
}

}  // namespace
}  // namespace mongo
