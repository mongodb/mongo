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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

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

TEST_F(QueryPlannerTest, ExprEqCanUseIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{$expr: {$eq: ['$a', 1]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {$expr: {$eq: ['$a', {$const: 1}]}}, node: {ixscan: {pattern: {a: 1}, "
        "bounds: {a: "
        "[[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, ExprEqCannotUseMultikeyFieldOfIndex) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("a.b" << 1), multikeyPaths);
    runQuery(fromjson("{$expr: {$eq: ['$a.b', 1]}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{$expr: {$eq: ['$a.b', 1]}}]}}}");
}

// Test that when a $expr predicate is ANDed with an index-eligible predicate, an imprecise
// $_internalExpr predicate is pushed into the IXSCAN bounds, to filter out results before fetching.
TEST_F(QueryPlannerTest, ExprQueryOnMultiKeyIndexPushesImpreciseFilterToIxscan) {
    params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a" << 1 << "b" << 1), false /* multikey */);
    runQuery(fromjson("{$and: [{a: 123}, {$expr: {$eq: ['$b', 456]}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{cscan: {filter: {$and: [{a: 123}, {$expr: {$eq: ['$b', 456]}}]}, dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$expr: {$eq: ['$b', 456]}}, node: "
        "{ixscan: {pattern: {a:1,b:1}, bounds: "
        "{a: [[123,123,true,true]], b: [[456,456,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenIndexKeyRequiredToCoverSortIsMultikey) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;

    // 'b' is multikey.
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, _id: 0}, "
                 "sort: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1, _id: 0}, node: "
        "{sort: {pattern: {b: 1}, limit: 0, type:'simple', node: "
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CoveredWhenMultikeyIndexComponentIsNotRequiredByQuery) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;

    // 'c' is multikey.
    MultikeyPaths multikeyPaths{{}, {}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1), multikeyPaths);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, _id: 0}, "
                 "sort: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1, _id: 0}, node: "
        "{sort: {pattern: {b: 1}, limit: 0, type:'default', node: "
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, CoveredWhenQueryOnNonMultikeyDottedPath) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;

    addIndex(BSON("a" << 1 << "b.c" << 1));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, 'b.c': 1, _id: 0}, "
                 "sort: {'b.c': 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {'b.c': 1}, limit: 0, type: 'default', node:"
        "{proj: {spec: {a: 1, 'b.c': 1, _id: 0}, node: "
        "{ixscan: {pattern: {a: 1, 'b.c': 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenFilterNonEmptyButMissingLeadingField) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a" << 1 << "b" << 1));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {b: {$gt: 0}}, projection: {a: 1, _id: 0}, "
                 "sort: {a: 1}}"));

    assertNumSolutions(1U);

    // A 'fetch' is required because we're not willing to push the {b: {$gt: 0}} predicate into
    // bounds without a predicate on the leading field.
    assertSolutionExists(
        "{proj: {spec: {a: 1, _id: 0}, node: {fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MustFetchWhenIndexKeyRequiredtoCoverProjectIsMultikey) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    // 'b' is multikey.
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {b: 1, _id: 0}, "
                 "sort: {a: 1}}"));

    assertNumSolutions(1U);

    assertSolutionExists(
        "{proj: {spec: {b: 1, _id: 0}, node: {fetch: {node: {ixscan: "
        "{pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CoveredWhenKeysAreNotMultikey) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    // 'b' is multikey.
    MultikeyPaths multikeyPaths{{}, {0U}, {}};
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1), multikeyPaths);

    // 'b' not used in query.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, _id: 0}, "
                 "sort: {c: 1}}"));

    assertNumSolutions(1U);

    assertSolutionExists(
        "{proj: {spec: {a: 1, _id: 0}, node: {sort: {pattern: {c: 1}, limit: 0, type:'default', "
        "node: {ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CanProduceCoveredSortPlanWhenSortOrderDifferentThanIndexKeyOrder) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 2}}, projection: {a: 1, b:1, _id: 0}, sort: "
                 "{b: 1, a: 1}}"));

    assertNumSolutions(1U);

    assertSolutionExists(
        "{sort: {pattern: {b: 1, a: 1}, limit: 0, type: 'default', node:"
        "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

// $eq can use a hashed index because it looks for values of type regex;
// it doesn't evaluate the regex itself.
TEST_F(QueryPlannerTest, EqCanUseHashedIndexWithRegex) {
    addIndex(BSON("a"
                  << "hashed"));
    runQuery(fromjson("{a: {$eq: /abc/}}"));
    ASSERT_EQUALS(getNumSolutions(), 2U);
}

TEST_F(QueryPlannerTest, ExprEqCanUseHashedIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a"
                  << "hashed"));
    runQuery(fromjson("{$expr: {$eq: ['$a', 1]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{$expr: {$eq: ['$a', {$const: 1}]}}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 'hashed'}}}}}");
}

TEST_F(QueryPlannerTest, ExprEqCanUseHashedIndexWithRegex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(BSON("a"
                  << "hashed"));
    runQuery(fromjson("{$expr: {$eq: ['$a', /abc/]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{$expr: {$eq: ['$a', {$const: /abc/}]}}]}, node: "
        "{ixscan: {filter: null, pattern: {a: 'hashed'}}}}}");
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

TEST_F(QueryPlannerTest, NotEqualsNullSparseIndex) {
    addIndex(BSON("x" << 1),
             false,  // multikey
             true    // sparse
    );

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {x: 1}},"
        "bounds: [['MinKey', undefined, true, false], [null, 'MaxKey', false, true]]}}}");
}

TEST_F(QueryPlannerTest, NotEqualsNullSparseMultiKeyIndex) {
    addIndex(BSON("x" << 1),
             true,  // multikey
             true   // sparse
    );

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, NotEqualsNullInElemMatchValueSparseMultiKeyIndex) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("x" << 1),
             true,  // multikey
             true   // sparse
    );

    runQuery(fromjson("{'x': {$elemMatch: {$ne: null}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {x: 1},"
        "bounds: {'x': [['MinKey', undefined, true, false], [null, 'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NotEqualsNullInElemMatchObjectSparseMultiKeyBelowElemMatch) {
    // "a.b.c" being multikey will prevent us from using the index since $elemMatch doesn't do
    // implicit array traversal.
    auto keyPattern = BSON("a.b.c.d" << 1);
    IndexEntry ind(keyPattern,
                   IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                   IndexDescriptor::kLatestIndexVersion,
                   true,
                   {},
                   {},
                   true,
                   false,
                   IndexEntry::Identifier{"ind"},
                   nullptr,  // filterExpr
                   BSONObj(),
                   nullptr,
                   nullptr);
    ind.multikeyPaths = {{2U}};
    addIndex(ind);

    runQuery(fromjson("{'a.b': {$elemMatch: {'c.d.': {$ne: null}}}}"));

    assertHasOnlyCollscan();
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

    runQuerySkipLimit(BSON("x" << 5), 3, 0);

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{skip: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
}

TEST_F(QueryPlannerTest, BasicSkipWithIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySkipLimit(BSON("a" << 5), 8, 0);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CoveredSkipWithIndex) {
    addIndex(fromjson("{a: 1, b: 1}"));

    runQuerySortProjSkipLimit(
        fromjson("{a: 5}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"), 8, 0);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5}}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, "
        "node: {skip: {n: 8, node: {ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, SkipEvaluatesAfterFetchWithPredicate) {
    addIndex(fromjson("{a: 1}"));

    runQuerySkipLimit(fromjson("{a: 5, b: 7}"), 8, 0);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{skip: {n: 8, node: {cscan: {dir: 1, filter: {a: 5, b: 7}}}}}");

    // When a plan includes a fetch with no predicate, the skip should execute first, so we avoid
    // fetching a document that we will always discard. When the fetch does have a predicate (as in
    // this case), however, that optimization would be invalid; we need to fetch the document and
    // evaluate the filter to determine if the document should count towards the number of skipped
    // documents.
    assertSolutionExists(
        "{skip: {n: 8, node: {fetch: {filter: {b: 7}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, SkipEvaluatesBeforeFetchForIndexedOr) {
    addIndex(fromjson("{a: 1}"));

    runQuerySkipLimit(fromjson("{$or: [{a: 5}, {a: 7}]}"), 8, 0);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{skip: {n: 8, node: "
        "{cscan: {dir: 1, filter: {a: {$in: [5, 7]}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, BasicLimitNoIndex) {
    addIndex(BSON("a" << 1));

    runQuerySkipLimit(BSON("x" << 5), 0, 3);

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{limit: {n: 3, node: {cscan: {dir: 1, filter: {x: 5}}}}}");
}

TEST_F(QueryPlannerTest, BasicLimitWithIndex) {
    addIndex(BSON("a" << 1 << "b" << 1));

    runQuerySkipLimit(BSON("a" << 5), 0, 5);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{limit: {n: 5, node: {cscan: {dir: 1, filter: {a: 5}}}}}");
    assertSolutionExists(
        "{limit: {n: 5, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, SkipAndLimit) {
    addIndex(BSON("x" << 1));

    runQuerySkipLimit(BSON("x" << BSON("$lte" << 4)), 7, 2);

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{limit: {n: 2, node: {skip: {n: 7, node: "
        "{cscan: {dir: 1, filter: {x: {$lte: 4}}}}}}}}");
    assertSolutionExists(
        "{limit: {n: 2, node: {fetch: {filter: null, node: "
        "{skip: {n: 7, node: {ixscan: {filter: null, pattern: {x: 1}}}}}}}}}");
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
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {}}}}}");
}

TEST_F(QueryPlannerTest, CantUseHashedIndexToProvideSort) {
    addIndex(BSON("x"
                  << "hashed"));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseHashedIndexToProvideSortWithIndexablePred) {
    addIndex(BSON("x"
                  << "hashed"));
    runQuerySortProj(BSON("x" << BSON("$in" << BSON_ARRAY(0 << 1))), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {node: "
        "{ixscan: {pattern: {x: 'hashed'}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {x: {$in: [0, 1]}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseTextIndexToProvideSort) {
    addIndex(BSON("x" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {}}}}}");
}

TEST_F(QueryPlannerTest, BasicSortWithIndexablePred) {
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {a: 5}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: "
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
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {a: 5}}}}}");
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
    // Negative limit indicates hard limit - see query_request_helper.cpp
    runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 0, 3);
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 3, type: 'simple', node:"
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, SortSkip) {
    runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 0);
    assertNumSolutions(1U);
    // If only skip is provided, do not limit sort.
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, SortSkipLimit) {
    runQuerySortProjSkipLimit(BSONObj(), fromjson("{a: 1}"), BSONObj(), 2, 3);
    assertNumSolutions(1U);
    // Limit in sort node should be adjusted by skip count
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 5, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}}}");
}

// Push project behind sort even when there is a skip between them.
TEST_F(QueryPlannerTest, PushProjectBehindSortWithSkipBetween) {
    runQueryAsCommand(fromjson(R"({
        find: 'testns',
        filter: {},
        sort: {a: 1},
        projection: {_id: 0, a: 1},
        skip: 2
    })"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{skip: {n: 2, node: "
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1}}}}}}}}}");
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
        "{sort: {pattern: {x: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1, filter: {x: {$gt: 1}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerTest, SortElimCompound) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{ a : 5 }"), BSON("b" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {a: 5}}}}}");
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
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {a: 5}}}}}");
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
        "{sort: {pattern: {c: -1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {a: 5, b: 6}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: "
        "{filter: null, dir: -1, pattern: {a: 1, b: 1, c: 1, d: 1}}}}}");
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

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: /foo/}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: /foo/}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {a: /foo/}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexAnd) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: /foo/, b: 2}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexAndCovering) {
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuerySortProj(fromjson("{a: /foo/, b: 2}"), BSONObj(), fromjson("{_id: 0, a: 1, b: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1, filter: {$and: [{b: 2}, {a: /foo/}]}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexOrCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$or: [{a: /0/}, {a: /1/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {a: {$in: [/0/, /1/]}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{ixscan: {filter: {a: {$in: [/0/, /1/]}}, pattern: {a: 1}}}}}");
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

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{b:/1/}]}}}}}");
}

TEST_F(QueryPlannerTest, TwoRegexSameFieldCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$and: [{a: /0/}, {a: /1/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/}]}}}}}");
}

TEST_F(QueryPlannerTest, ThreeRegexSameFieldCovering) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(
        fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"), BSONObj(), fromjson("{_id: 0, a: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: "
        "{cscan: {dir: 1, filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}}}}}");
}

TEST_F(QueryPlannerTest, NonPrefixRegexMultikey) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: /foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {filter: {a: /foo/}, dir: 1}}");
}

TEST_F(QueryPlannerTest, ThreeRegexSameFieldMultikey) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{$and: [{a: /0/}, {a: /1/}, {a: /2/}]}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {filter: {$and:[{a:/0/},{a:/1/},{a:/2/}]}, dir: 1}}");
}

//
// Negation
//

TEST_F(QueryPlannerTest, NegationIndexForSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: 1}}"), fromjson("{a: 1}"), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
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

// Negated $eq: <Array> won't use the index.
TEST_F(QueryPlannerTest, NegationEqArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$eq: [1, 2]}}}"));

    assertHasOnlyCollscan();
}

// If we negate a $in and any of the members of the $in equalities
// is an array, we don't use the index.
TEST_F(QueryPlannerTest, NegationInArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$in: [1, [1, 2]]}}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, ElemMatchValueNegationEqArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$elemMatch: {$not: {$eq: [1]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, ElemMatchValueNegationInArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$elemMatch: {$not: {$in: [[1]]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegatedElemMatchValueEqArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {$eq: [1]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegatedElemMatchValueInArray) {
    addIndex(BSON("i" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {$in: [[1]]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, ElemMatchObjectNegationEqArray) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$elemMatch: {j: {$ne: [1]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, ElemMatchObjectNegationInArray) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$elemMatch: {j: {$not: {$in: [[1]]}}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegatedElemMatchObjectEqArray) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {j: [1]}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerTest, NegatedElemMatchObjectInArray) {
    addIndex(BSON("i.j" << 1));
    runQuery(fromjson("{i: {$not: {$elemMatch: {j: {$in: [[1]]}}}}}"));
    assertHasOnlyCollscan();
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

TEST_F(QueryPlannerTest, NENull) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, bounds: {a: "
        "[['MinKey',undefined,true,false],[null,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NinNull) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$nin: [null, 4]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',undefined,true,false],"
        "[null,4,false,false],"
        "[4,'MaxKey',false,true]]}}}}}");
}


TEST_F(QueryPlannerTest, NENullWithSort) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: null}}"), BSON("a" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {filter: {a: {$ne: null}}, dir: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',undefined,true,false],"
        "[null,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NENullWithProjection) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: null}}"), BSONObj(), BSON("_id" << 0 << "a" << 1));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {ixscan: {pattern: {a:1}, "
        "bounds: {a: [['MinKey',undefined,true,false],[null,'MaxKey',false,true]]}}}}}");
}

TEST_F(QueryPlannerTest, NENullWithSortAndProjection) {
    addIndex(BSON("a" << 1));
    runQuerySortProj(fromjson("{a: {$ne: null}}"), BSON("a" << 1), BSON("_id" << 0 << "a" << 1));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {"
        "  ixscan: {pattern: {a:1}, bounds: {"
        "    a: [['MinKey',undefined,true,false], [null,'MaxKey',false,true]]"
        "}}}}}");
}

// In general, a negated $nin can make use of an index.
TEST_F(QueryPlannerTest, NinUsesMultikeyIndex) {
    // 'true' means multikey.
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
TEST_F(QueryPlannerTest, NinWithRegexCantUseMultikeyIndex) {
    // 'true' means multikey.
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$nin: [4, /foobar/]}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// Or if it contains a null, because null is "equal" to undefined, and undefined can represent an
// empty array. Therefore negating the bounds [undefined, null], would lead to the query missing
// values for empty array.
TEST_F(QueryPlannerTest, NinWithNullCantUseMultikeyIndex) {
    // 'true' means multikey.
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$nin: [4, null]}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, FetchAfterSortWhenOnlyProjectNeedsDocuments) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$gt: 0}}, projection: {a: 1, b:1, c:1, _id: 0}, "
                 "sort: {b: 1, a:1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: 1, b:1, c:1, _id: 0}, node: {fetch: {node: {sort: {pattern: {b: 1, a: "
        "1}, limit: 0, type: 'default', node: "
        "{ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, ExclusionProjectionCanSwapBeneathSort) {
    runQueryAsCommand(fromjson("{find: 'testns', projection: {a: 0, b: 0}, sort: {c: 1, d: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {c: 1, d: 1}, limit: 0, type: 'simple', node:"
        "{proj: {spec: {a: 0, b: 0}, node: {cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, ProjectionWithExpressionDoesNotSwapBeneathSort) {
    runQueryAsCommand(
        fromjson("{find: 'testns', "
                 "projection: {_id: 0, a: 1, b: 1, c: {$add: ['$c', '$e']}}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1, c: {$add: ['$c', '$e']}}, node:"
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node:"
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, ProjectionWithConstantExpressionDoesNotSwapBeneathSort) {
    runQueryAsCommand(
        fromjson("{find: 'testns', "
                 "projection: {_id: 0, a: 1, b: 1, c: 'constant'}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1, c: 'constant'}, node:"
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, InclusionProjectionCannotSwapBeneathSortIfItExcludesSortedOnField) {
    runQueryAsCommand(fromjson("{find: 'testns', projection: {_id: 0, a: 1}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node:"
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, ExclusionProjectionCannotSwapBeneathSortIfItExcludesSortedOnField) {
    runQueryAsCommand(fromjson("{find: 'testns', projection: {b: 0}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {b: 0}, node:"
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, ProjectionDoesNotSwapBeforeSortWithLimit) {
    runQueryAsCommand(
        fromjson("{find: 'testns', projection: {_id: 0, a: 1, b: 1}, sort: {a: 1}, limit: 3}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node:"
        "{sort: {pattern: {a: 1}, limit: 3, type: 'simple', node: "
        "{cscan: {dir: 1}}}}}}");
}

}  // namespace
}  // namespace mongo
