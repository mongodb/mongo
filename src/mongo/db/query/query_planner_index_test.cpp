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

#include <cstddef>
#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {


TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanForCountWhenIndexSatisfiesQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    addIndex(BSON("x" << 1));
    runQuery(BSON("x" << 5));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{ixscan: {pattern: {x: 1}, bounds: {x: [[5,5,true,true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchToIxscanForCountWhenFetchFilterNonempty) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    addIndex(BSON("x" << 1));
    runQuery(BSON("y" << 3 << "x" << 5));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {y: 3}, node: {ixscan: "
        "{pattern: {x: 1}, bounds: {x: [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanForCountWhenIndexSatisfiesNullQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    addIndex(BSON("x" << 1));
    runQuery(fromjson("{x: null}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{ixscan: {pattern: {x: 1}, bounds: "
        "{x: [[null,null,true,true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanWhenIndexSatisfiesNullAndOtherQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    addIndex(BSON("x" << 1));
    runQuery(fromjson("{x: {$in: [null, 2]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{ixscan: {pattern: {x: 1}, bounds: "
        "{x: [[null,null,true,true], [2, 2, true, true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanCountWhenMultikeyIndexSatisfiesNullQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(fromjson("{x: 1}"), multikeyPaths);
    runQuery(fromjson("{x: null}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{ixscan: {pattern: {x: 1}, bounds: {x: [[null,null,true,true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchWhenMultikeyIndexSatisfiesDottedNullQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(fromjson("{'x.y': 1}"), multikeyPaths);
    runQuery(fromjson("{'x.y': null}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {'x.y': null}, node: {ixscan: {pattern: {'x.y': 1}, bounds: {'x.y': "
        "[[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchForCountWhenMultikeyIndexSatisfiesNullEmptyQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(fromjson("{x: 1}"), multikeyPaths);
    runQuery(fromjson("{x: {$in: [null, []]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$in: [null, []]}}, node: {ixscan: {pattern: {x: 1}, bounds: "
        "{x: [[undefined,undefined,true,true], [null,null,true,true], [[], [], true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchWhenMultikeyIndexSatisfiesNullEmptyAndOtherQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(fromjson("{x: 1}"), multikeyPaths);
    runQuery(fromjson("{x: {$in: [null, [], 2]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$in: [null, [], 2]}}, node: {ixscan: {pattern: {x: 1}, bounds: "
        "{x: [[undefined,undefined,true,true], [null,null,true,true], [2, 2, true, true], [[], [], "
        "true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanWhenMultikeyIndexSatisfiesNullAndOtherQuery) {
    params.options = QueryPlannerParams::DEFAULT;
    setIsCountLike();
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(fromjson("{x: 1}"), multikeyPaths);
    runQuery(fromjson("{x: {$in: [null, 2]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{ixscan: {pattern: {x: 1}, bounds: "
        "{x: [[null,null,true,true], [2, 2, true, true]]}}}");
}

TEST_F(QueryPlannerTest, PlannerUsesCoveredIxscanFindWhenIndexSatisfiesNullQuery) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{x: 1, _id: 1}"));
    runQuerySortProj(fromjson("{x: null}}"), BSONObj(), fromjson("{_id: 1}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node:"
        "{ixscan: {pattern: {x: 1, _id: 1}, bounds: "
        "{x: [[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchForFindWhenMultikeyIndexSatisfiesNullQuery) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(fromjson("{x: 1, _id: 1}"), multikeyPaths);
    runQuerySortProj(fromjson("{x: null}}"), BSONObj(), fromjson("{_id: 1}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node: {ixscan: {pattern: {x: 1, _id: 1}, bounds: "
        "{x: [[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerAddsFetchFindWhenMultikeyIndexSatisfiesNullEmptyQuery) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(fromjson("{x: 1, _id: 1}"), multikeyPaths);
    runQuerySortProj(fromjson("{x: {$in: [null, []]}}}"), BSONObj(), fromjson("{_id: 1}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node:"
        "{fetch: {filter: {x: {$in: [null, []]}}, node: {ixscan: {pattern: {x: 1, _id: 1}, bounds: "
        "{x: [[undefined,undefined,true,true],[null,null,true,true], [[], [], true, true]]}}}}}}}");
}

TEST_F(QueryPlannerTest,
       PlannerUsesCoveredIxscanForFindWhenMultikeyIndexSatisfiesNullAndOtherQuery) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(fromjson("{x: 1, _id: 1}"), multikeyPaths);
    runQuerySortProj(fromjson("{x: {$in: [null, 2]}}}"), BSONObj(), fromjson("{_id: 1}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 1}, node:"
        "{ixscan: {pattern: {x: 1, _id: 1}, bounds: "
        "{x: [[null,null,true,true], [2, 2, true, true]]}}}}}");
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
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
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
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1}}}}");
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

TEST_F(QueryPlannerTest, ExprEqCannotUseSparseIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1}"), false, true);
    runQuery(fromjson("{a: {$_internalExprEq: 1}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, ExprEqCannotUseSparseIndexForEqualityToNull) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1}"), false, true);
    runQuery(fromjson("{a: {$_internalExprEq: null}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegationCannotUseSparseIndex) {
    // Sparse indexes can't support negation queries because they are sparse, and {a: {$ne: 5}}
    // will match documents which don't have an "a" field.
    addIndex(fromjson("{a: 1}"),
             false,  // multikey
             true    // sparse
    );
    runQuery(fromjson("{a: {$ne: 5}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$not: {$gt: 3, $lt: 5}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegationInElemMatchDoesNotUseSparseIndex) {
    // Logically, there's no reason a sparse index could not support a negation inside a
    // "$elemMatch value", but it is not something we've implemented.
    addIndex(fromjson("{a: 1}"),
             true,  // multikey
             true   // sparse
    );
    runQuery(fromjson("{a: {$elemMatch: {$ne: 5}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$elemMatch: {$not: {$gt: 3, $lt: 5}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NinListWithOnlyNullAndEmptyArrayShouldUseMultikeyIndex) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    // Use a multikey index.
    addIndex(fromjson("{a: 1}"), true);
    runQuery(fromjson("{a: { $nin: [[], null] }}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$not: {$in: [null, []]}}}, node: {ixscan: "
        "{pattern: {a: 1}, bounds: "
        "{a: ["
        "['MinKey', null, true, false],"
        "[null, [], false, false],"
        "[[], 'MaxKey', false, true]"
        "]}}}}}");
}

TEST_F(QueryPlannerTest, NinListWithOnlyNullAndEmptyArrayShouldUseIndex) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    // Use an index which is not multikey.
    addIndex(fromjson("{a: 1}"));
    runQuery(fromjson("{a: { $nin: [[], null] }}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: "
        "{pattern: {a: 1}, bounds: "
        "{a: ["
        "['MinKey', null, true, false],"
        "[null, [], false, false],"
        "[[], 'MaxKey', false, true]"
        "]}}}}}");
}

TEST_F(QueryPlannerTest, NinListWithNullShouldNotUseIndex) {
    addIndex(fromjson("{a: 1}"), true);
    runQuery(fromjson("{a: { $nin: [null] }}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NinListWithRegexCannotUseIndex) {
    addIndex(fromjson("{a: 1}"), true);
    // This matches the [[], null] pattern but also has a regex.
    runQuery(fromjson("{a: { $nin: [[], null, /abc/] }}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NinListWithNonEmptyArrayShouldNotUseIndex) {
    addIndex(fromjson("{a: 1}"), true);
    runQuery(fromjson("{a: { $nin: [[], [1]] }}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NotLtEmptyArrayShouldNotUseIndex) {
    addIndex(fromjson("{a: 1}"), true);
    runQuery(fromjson("{a: { $not: { $lt: [] } } }"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, SparseIndexCannotSupportEqualsNull) {
    addIndex(BSON("i" << 1),
             false,  // multikey
             true    // sparse
    );

    runQuery(fromjson("{i: {$eq: null}}"));
    assertHasOnlyCollscan();
}

// TODO: SERVER-37164: The semantics of {$gte: null} and {$lte: null} are inconsistent with and
// without a sparse index. It is unclear whether or not a sparse index _should_ be able to support
// these operations.
TEST_F(QueryPlannerTest, SparseIndexCanSupportGTEOrLTENull) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("i" << 1),
             false,  // multikey
             true    // sparse
    );

    runQuery(fromjson("{i: {$gte: null}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {i: {$gte: null}}, node: {ixscan: {pattern: "
        "{i: 1}, bounds: {i: [[null,null,true,true]]}}}}}");

    runQuery(fromjson("{i: {$lte: null}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {i: {$lte: null}}, node: {ixscan: {pattern: "
        "{i: 1}, bounds: {i: [[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, PlannerCanUseIndexesWithSameKeyButDifferentSparseProperty) {
    // Create two indexes on the same key pattern; one sparse, the other non-sparse. This is
    // permitted because the 'sparse' property is part of the index signature.
    addIndex(fromjson("{a: 1}"), /*multikey*/ false, /*sparse*/ false);
    addIndex(fromjson("{a: 1}"), /*multikey*/ false, /*sparse*/ true, /*unique*/ false, "a_sparse");

    runQuery(fromjson("{a: 1}"));

    // Plan #1: FETCH > IXSCAN with a_1 index.
    // Plan #2: FETCH > IXSCAN with a_sparse index.
    // Plan #3: COLLSCAN with filter a == 1.
    assertNumSolutions(3U);

    // There must be a solution that uses the sparse index "a_sparse".
    assertSolutionExists("{fetch: {node: {ixscan: {name: \"a_sparse\"}}}}");
}

TEST_F(QueryPlannerTest, PlannerCanUseIndexesWithSameKeyButDifferentUniqueProperty) {
    // Create two indexes on the same key pattern; one unique, the other non-unique. This is
    // permitted because the 'unique' property is part of the index signature.
    addIndex(fromjson("{a: 1}"), /*multikey*/ false, /*sparse*/ false, /*unique*/ false);
    addIndex(fromjson("{a: 1}"), /*multikey*/ false, /*sparse*/ false, /*unique*/ true, "a_unique");

    runQuery(fromjson("{a: 1}"));

    // Plan #1: FETCH > IXSCAN with a_1 index.
    // Plan #2: FETCH > IXSCAN with a_unique index.
    // Plan #3: COLLSCAN with filter a == 1.
    assertNumSolutions(3U);

    // There must be a solution that uses the unique index "a_unique".
    assertSolutionExists("{fetch: {node: {ixscan: {name: \"a_unique\"}}}}");
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
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: {ixscan: {filter: null, pattern: {'a.b': 1},"
        "bounds: {'a.b': [[5,5,true,true]]}}}}}");
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

TEST_F(QueryPlannerTest, CompoundIndexBoundsNotEqualsNull) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: {$gt: 'foo'}, b: {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
        "{a: 1, b: 1}, bounds: {a: [['foo',{},false,false]], "
        "b:[['MinKey',null,true,false],[null,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNull) {
    addIndex(BSON("a.b" << 1 << "c.d" << 1));
    runQuery(fromjson("{'a.b': {$gt: 'foo'}, 'c.d': {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: "
        "{'a.b': 1, 'c.d': 1}, bounds: {'a.b': [['foo',{},false,false]], "
        "'c.d':[['MinKey',null,true,false],[null,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNullWithProjection) {
    addIndex(BSON("a.b" << 1 << "c.d" << 1));
    runQuerySortProj(fromjson("{'a.b': {$gt: 'foo'}, 'c.d': {$ne: null}}"),
                     BSONObj(),
                     fromjson("{_id: 0, 'c.d': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, 'c.d': 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'c.d': 1}, node: {"
        "  ixscan: {filter: null, pattern: {'a.b': 1, 'c.d': 1}, bounds: {"
        "    'a.b': [['foo',{},false,false]], "
        "    'c.d':[['MinKey',null,true,false],[null,'MaxKey',false,true]]"
        "}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsAndWithNestedOr) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$and: [{a: 1, $or: [{a: 2}, {a: 3}]}]}"));

    // Given that the index over 'a' isn't multikey, we ideally won't generate any solutions
    // since we know the query describes an empty set if 'a' isn't multikey.  Any solutions
    // below are "this is how it currently works" instead of "this is how it should work."

    // It's kind of iffy to look for indexed solutions so we don't...
    size_t matches = 0;
    matches += numSolutionMatches("{cscan: {dir: 1, filter: {$or: [{a: 2, a:1}, {a: 3, a:1}]}}}");
    matches +=
        numSolutionMatches("{cscan: {dir: 1, filter: {$and: [{a: {$in: [2, 3]}}, {a: 1}]}}}");
    ASSERT_GREATER_THAN_OR_EQUALS(matches, 1U);
}

TEST_F(QueryPlannerTest, IndexBoundsIndexedSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("a" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a:1}, limit: 0, type: 'simple', node: "
        "{cscan: {filter: {a: {$in: [1,2]}}, dir: 1}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsUnindexedSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, type: 'simple', node: "
        "{cscan: {filter: {a: {$in: [1,2]}}, dir: 1}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, type: 'simple', node: {fetch: "
        "{filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, IndexBoundsUnindexedSortHint) {
    addIndex(BSON("a" << 1));
    runQuerySortHint(fromjson("{$or: [{a: 1}, {a: 2}]}"), BSON("b" << 1), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b:1}, limit: 0, type: 'simple', node: {fetch: "
        "{filter: null, node: {ixscan: {filter: null, "
        "pattern: {a:1}, bounds: {a: [[1,1,true,true], [2,2,true,true]]}}}}}}}");
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
    runQuery(fromjson("{$or: [{a: {$ne: null}}, {a: {$ne: 4}}]}"));

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

TEST_F(QueryPlannerTest, NoTableScanBasic) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    runInvalidQuery(BSONObj());
    assertNoSolutions();

    addIndex(BSON("x" << 1));
    runInvalidQuery(BSONObj());
    assertNoSolutions();

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
        "{fetch: {filter: {a: 1, b: {$gt: 1}}, node: {andHash: {nodes: ["
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
        "{fetch: {filter: {a: 1, b: 1, c: 1}, node: {andSorted: {nodes: ["
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
        "{fetch: {filter: {a: 1, b: 1}, node: {andSorted: {nodes: ["
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
        "{fetch: {filter: {a: 1, b: 1}, node: {andSorted: {nodes: ["
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
        "{fetch: {node: {andSorted: {nodes: ["
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

TEST_F(QueryPlannerTest, CannotIntersectSubnodes) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("c" << 1));
    addIndex(BSON("d" << 1));

    runQuery(fromjson("{$or: [{a: 1}, {b: 1}], $or: [{c: 1}, {d: 1}]}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {$or: [{c: 1}, {d: 1}]}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1}}},"
        "{ixscan: {filter: null, pattern: {b: 1}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: 1}, {b: 1}]}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {c: 1}}},"
        "{ixscan: {filter: null, pattern: {d: 1}}}"
        "]}}}}");
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
        "{fetch: {filter: {a:1,$or:[{b:1},{c:1}]}, node: {or: {nodes: ["
        "{andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a':1}}},"
        "{ixscan: {filter: null, pattern: {'b':1}}}]}},"
        "{andSorted: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a':1}}},"
        "{ixscan: {filter: null, pattern: {'c':1}}}]}}]}}}}");
    matches += numSolutionMatches(
        "{fetch: {filter: {a:1,$or:[{b:1},{c:1}]}, node: {andHash: {nodes:["
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
        "{fetch: {filter: {a: 1, b: {$gt: 1}}, node: {andHash: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}},"
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Rearrange the preds, shouldn't matter.
    runQuerySortProj(fromjson("{b: 1, a:{$lt: 7}}"), fromjson("{b:1}"), BSONObj());
    assertSolutionExists(
        "{fetch: {filter: {b: 1, a: {$lt: 7}}, node: {andHash: {nodes: ["
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
        "{fetch: {filter: {a:{$gt:1}, b: 1, c: 1}, node: {andSorted: {nodes: ["
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


TEST_F(QueryPlannerTest, NoFetchStageWhenSingleFieldSortIsCoveredByIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Sort on 'b'.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, _id: 0}, "
                 "sort: {b: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'default', node:"
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node:"
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, NoFetchStageWhenTwoFieldAscendingSortIsCoveredByIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Sort on 'b', 'a'.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, _id: 0}, "
                 "sort: {b: 1, a: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1, a: 1}, limit: 0, type: 'default', node:"
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node:"
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, NoFetchStageWhenTwoFieldMixedSortOrderSortIsCoveredByIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Sort on 'b', 'a' descending.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, _id: 0}, "
                 "sort: {b: 1, a: -1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1, a: -1}, limit: 0, type: 'default', node:"
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenNotAllSortKeysAreCoveredByIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, _id: 0}, "
                 "sort: {b: 1, c: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1, b:1, _id: 0}, node: "
        "{sort: {pattern: {b: 1, c: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: {ixscan: "
        "{pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, NoFetchStageWhenProjectionUsesExpressionWithCoveredDependency) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, a: {$add: ['$a', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: {$add: ['$a', 1]}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest,
       NoFetchStageWhenProjectionAssignsExpressionWithCoveredDependencyToUnIndexedField) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Assign the result of the expression to 'x' which has no index.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, x: {$add: ['$a', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: {$add: ['$a', 1]}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, AddsFetchWhenProjectionAssignsToUnindexedDottedField) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Cannot be covered since 'x' may be an array.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, 'x.y': {$add: ['$a', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'x.y': {$add: ['$a', 1]}}, node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, NoFetchWhenProjectionAssignsToDottedIndexedNonArrayField) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1, 'b.c': 1}"));

    // Can be covered since 'b' and 'b.c' are known to not be multikey.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, 'b.c': {$add: ['$a', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'b.c': {$add: ['$a', 1]}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1, 'b.c': 1}}}}}");
}

TEST_F(QueryPlannerTest, NoFetchWhenProjectionAssignsToIndexedNonArrayField) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    // Can be covered since 'b' is known to not be multikey.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, b: {$add: ['$a', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, b: {$add: ['$a', 1]}}, node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenExpressionUsesMultiKeyPath) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"), {{}, {0}});

    // Cannot be covered since 'b' is multikey.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, x: {$add: ['$b', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: {$add: ['$b', 1]}}, node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenExpressionUsesDottedMultiKeyPath) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"), {{}, {0}});

    // Cannot be covered since 'b' is multikey, meaning the result of the expression '$b.c' could
    // result in an array.
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, x: {$add: ['$b.c', 1]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: {$add: ['$b.c', 1]}}, node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenExpressionUsesROOT) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"), {{}, {0}});

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {_id: 0, x: '$$ROOT'}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: '$$ROOT'}, node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, PlansForWholeIndexScanWithSortAreGenerated) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {}, projection: {'b': 1, _id: 0}, sort: {'a': 1}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {'b': 1, _id: 0}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
    assertSolutionExists(
        "{proj: {spec: {'b': 1, _id: 0}, node: {fetch: {node: {ixscan: {pattern: {a: 1}}}}}}}");
}

}  // namespace
}  // namespace mongo
