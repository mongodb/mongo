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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace {

using namespace mongo;

TEST_F(QueryPlannerTest, ElemMatchOneField) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{a : {$elemMatch: {b:1}}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:1}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchTwoFields) {
    addIndex(BSON("a.b" << 1));
    addIndex(BSON("a.c" << 1));
    runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a:{$elemMatch:{b:1,c:1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'a.c': 1}}}}}");
}

TEST_F(QueryPlannerTest, BasicAllElemMatch) {
    addIndex(BSON("foo.a" << 1));
    addIndex(BSON("foo.b" << 1));
    runQuery(fromjson("{foo: {$all: [ {$elemMatch: {a:1, b:1}}, {$elemMatch: {a:2, b:2}}]}}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: {foo:{$all:"
        "[{$elemMatch:{a:1,b:1}},{$elemMatch:{a:2,b:2}}]}}}}");

    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'foo.a': 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {'foo.b': 1}}}}}");
}

TEST_F(QueryPlannerTest, BasicAllElemMatch2) {
    // true means multikey
    addIndex(BSON("a.x" << 1), true);

    runQuery(fromjson("{a: {$all: [{$elemMatch: {x: 3}}, {$elemMatch: {y: 5}}]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$all:[{$elemMatch:{x:3}},{$elemMatch:{y:5}}]}},"
        "node: {ixscan: {pattern: {'a.x': 1},"
        "bounds: {'a.x': [[3,3,true,true]]}}}}}");
}

// SERVER-16256
TEST_F(QueryPlannerTest, AllElemMatchCompound) {
    // true means multikey
    addIndex(BSON("d" << 1 << "a.b" << 1 << "a.c" << 1), true);

    runQuery(
        fromjson("{d: 1, a: {$all: [{$elemMatch: {b: 2, c: 2}},"
                 "{$elemMatch: {b: 3, c: 3}}]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 2, c: 2}}},"
        "{a: {$elemMatch: {b: 3, c: 3}}}]},"
        "node: {ixscan: {filter: null, pattern: {d:1,'a.b':1,'a.c':1},"
        "bounds: {d: [[1,1,true,true]],"
        "'a.b': [[2,2,true,true]],"
        "'a.c': [[2,2,true,true]]}}}}}");
}

// SERVER-13677
TEST_F(QueryPlannerTest, ElemMatchWithAllElemMatchChild) {
    addIndex(BSON("a.b.c.d" << 1));
    runQuery(fromjson("{z: 1, 'a.b': {$elemMatch: {c: {$all: [{$elemMatch: {d: 0}}]}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c.d': 1}}}}}");
}

// SERVER-13677
TEST_F(QueryPlannerTest, ElemMatchWithAllElemMatchChild2) {
    // true means multikey
    addIndex(BSON("a.b.c.d" << 1), true);
    runQuery(
        fromjson("{'a.b': {$elemMatch: {c: {$all: "
                 "[{$elemMatch: {d: {$gt: 1, $lt: 3}}}]}}}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c.d': 1}, "
        "bounds: {'a.b.c.d': [[-Infinity,3,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c.d': 1}, "
        "bounds: {'a.b.c.d': [[1,Infinity,false,true]]}}}}}");
}

// SERVER-13677
TEST_F(QueryPlannerTest, ElemMatchWithAllChild) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1), true);
    runQuery(fromjson("{z: 1, 'a.b': {$elemMatch: {c: {$all: [4, 5, 6]}}}}"));

    assertNumSolutions(4U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, "
        "bounds: {'a.b.c': [[4,4,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, "
        "bounds: {'a.b.c': [[5,5,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, "
        "bounds: {'a.b.c': [[6,6,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchValueMatch) {
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << 1 << "bar" << 1));
    runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));

    assertNumSolutions(3);
    assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$elemMatch:{$gt:5,$lt:10}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {foo: {$elemMatch: {$gt: 5, $lt: 10}}}, node: "
        "{ixscan: {filter: null, pattern: {foo: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {foo: {$elemMatch: {$gt: 5, $lt: 10}}}, node: "
        "{ixscan: {filter: null, pattern: {foo: 1, bar: 1}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchValueIndexability) {
    addIndex(BSON("foo" << 1));

    // An ELEM_MATCH_VALUE can be indexed if all of its child predicates
    // are "index bounds generating".
    runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $lt: 10}}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$elemMatch:{$gt:5,$lt:10}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {foo: {$elemMatch: {$gt: 5, $lt: 10}}}, node: "
        "{ixscan: {filter: null, pattern: {foo: 1}}}}}");

    // We cannot build index bounds for the $size predicate. This means that the
    // ELEM_MATCH_VALUE is not indexable, and we get no indexed solutions.
    runQuery(fromjson("{foo: {$elemMatch: {$gt: 5, $size: 10}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{cscan: {dir: 1, filter: {foo:{$elemMatch:{$gt:5,$size:10}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchNested) {
    addIndex(BSON("a.b.c" << 1));
    runQuery(fromjson("{ a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } }}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
}

TEST_F(QueryPlannerTest, TwoElemMatchNested) {
    addIndex(BSON("a.d.e" << 1));
    addIndex(BSON("a.b.c" << 1));
    runQuery(
        fromjson("{ a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },"
                 "b:{ $elemMatch:{ c:{ $gte:1 } } } } } }"));

    ASSERT_EQUALS(getNumSolutions(), 3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.d.e': 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchCompoundTwoFields) {
    addIndex(BSON("a.b" << 1 << "a.c" << 1));
    runQuery(fromjson("{a : {$elemMatch: {b:1, c:1}}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}}}}}");
}

TEST_F(QueryPlannerTest, ArrayEquality) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a : [1, 2, 3]}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a:[1,2,3]}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:[1,2,3]}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}}}}}");
}

// SERVER-13664
TEST_F(QueryPlannerTest, ElemMatchEmbeddedAnd) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: {$gte: 2, $lt: 4}, c: 25}}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:2,$lt: 4},c:25}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[-Infinity,4,true,false]], "
        "'a.c': [[25,25,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:2,$lt: 4},c:25}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2,Infinity,true,true]], "
        "'a.c': [[25,25,true,true]]}}}}}");
}

// SERVER-13664
TEST_F(QueryPlannerTest, ElemMatchEmbeddedOr) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    // true means multikey
    addIndex(BSON("a.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {$or: [{b: 3}, {c: 4}]}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{$or:[{b:3},{c:4}]}}}, "
        "node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}, "
        "{ixscan: {filter: null, pattern: {'a.c': 1}}}]}}}}");
}

// SERVER-13664
TEST_F(QueryPlannerTest, ElemMatchEmbeddedRegex) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{a: {$elemMatch: {b: /^foo/}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:/^foo/}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

// SERVER-14180
TEST_F(QueryPlannerTest, ElemMatchEmbeddedRegexAnd) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{a: {$elemMatch: {b: /^foo/}}, z: 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:/^foo/}}, z:1}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

// SERVER-14180
TEST_F(QueryPlannerTest, ElemMatchEmbeddedRegexAnd2) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{a: {$elemMatch: {b: /foo/, b: 3}}, z: 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:/foo/,b:3}}, z:1}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

// $not can appear as a value operator inside of an elemMatch (value).  We shouldn't crash if we
// see it.
TEST_F(QueryPlannerTest, ElemMatchWithNotInside) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$elemMatch: {$not: {$gte: 6}}}}"));
}

// SERVER-13789
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedOr) {
    addIndex(BSON("bar.baz" << 1));
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {$or: [{baz: 2}]}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{foo:1},"
        "{bar:{$elemMatch:{baz:2}}}]}, "
        "node: {ixscan: {pattern: {'bar.baz': 1}, "
        "bounds: {'bar.baz': [[2,2,true,true]]}}}}}");
}

// SERVER-13789
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedOrMultiplePreds) {
    addIndex(BSON("bar.baz" << 1));
    addIndex(BSON("bar.z" << 1));
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {$or: [{baz: 2}, {z: 3}]}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{foo:1},"
        "{bar:{$elemMatch:{$or:[{baz:2},{z:3}]}}}]}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'bar.baz': 1}, "
        "bounds: {'bar.baz': [[2,2,true,true]]}}},"
        "{ixscan: {pattern: {'bar.z': 1}, "
        "bounds: {'bar.z': [[3,3,true,true]]}}}]}}}}");
}

// SERVER-13789: Ensure that we properly compound in the multikey case when an
// $or is beneath an $elemMatch.
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedOrMultikey) {
    // true means multikey
    addIndex(BSON("bar.baz" << 1 << "bar.z" << 1), true);
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {$or: [{baz: 2, z: 3}]}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{foo:1},"
        "{bar: {$elemMatch: {$and: [{baz:2}, {z:3}]}}}]},"
        "node: {ixscan: {pattern: {'bar.baz': 1, 'bar.z': 1}, "
        "bounds: {'bar.baz': [[2,2,true,true]],"
        "'bar.z': [[3,3,true,true]]}}}}}");
}

// SERVER-13789: Right now we don't index $nor, but make sure that the planner
// doesn't get confused by a $nor beneath an $elemMatch.
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedNor) {
    addIndex(BSON("bar.baz" << 1));
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {$nor: [{baz: 2}, {baz: 3}]}}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// SERVER-13789: Make sure we properly handle an $or below $elemMatch that is not
// tagged by the enumerator to use an index.
TEST_F(QueryPlannerTest, ElemMatchNestedOrNotIndexed) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{c: 1, a: {$elemMatch: {b: 3, $or: [{c: 4}, {c: 5}]}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[3,3,true,true]]}}}}}");
}

// The index bounds can be compounded because the index is not multikey.
TEST_F(QueryPlannerTest, CompoundBoundsElemMatchNotMultikey) {
    addIndex(BSON("a.x" << 1 << "a.b.c" << 1));
    runQuery(fromjson("{'a.x': 1, a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1}}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:{$elemMatch:{c:{$gte:1}}}}}}, "
        "node: {ixscan: {pattern: {'a.x':1, 'a.b.c':1}, bounds: "
        "{'a.x': [[1,1,true,true]], "
        " 'a.b.c': [[1,Infinity,true,true]]}}}}}");
}

// The index bounds cannot be compounded because the predicates over 'a.x' and
// 'a.b.c' 1) share the prefix "a", and 2) are not conjoined by an $elemMatch
// over the prefix "a".
TEST_F(QueryPlannerTest, CompoundMultikeyBoundsElemMatch) {
    // true means multikey
    addIndex(BSON("a.x" << 1 << "a.b.c" << 1), true);
    runQuery(fromjson("{'a.x': 1, a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1}}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.x':1, 'a.b.c':1}, bounds: "
        "{'a.x': [[1,1,true,true]], "
        " 'a.b.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// The index bounds cannot be intersected because the index is multikey.
// The bounds could be intersected if there was an $elemMatch applied to path
// "a.b.c". However, the $elemMatch is applied to the path "a.b" rather than
// the full path of the indexed field.
TEST_F(QueryPlannerTest, MultikeyNestedElemMatch) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1, $lte: 1}}}}}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, bounds: "
        "{'a.b.c': [[-Infinity, 1, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, bounds: "
        "{'a.b.c': [[1, Infinity, true, true]]}}}}}");
}

// The index bounds cannot be intersected because the index is multikey.
// The bounds could be intersected if there was an $elemMatch applied to path
// "a.b.c". However, the $elemMatch is applied to the path "a.b" rather than
// the full path of the indexed field.
TEST_F(QueryPlannerTest, MultikeyNestedElemMatchIn) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1, $in:[2]}}}}}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, bounds: "
        "{'a.b.c': [[1, Infinity, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, bounds: "
        "{'a.b.c': [[2, 2, true, true]]}}}}}");
}

// The bounds can be compounded because the index is not multikey.
TEST_F(QueryPlannerTest, TwoNestedElemMatchBounds) {
    addIndex(BSON("a.d.e" << 1 << "a.b.c" << 1));
    runQuery(
        fromjson("{a: {$elemMatch: {d: {$elemMatch: {e: {$lte: 1}}},"
                 "b: {$elemMatch: {c: {$gte: 1}}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.d.e': 1, 'a.b.c': 1}, bounds: "
        "{'a.d.e': [[-Infinity, 1, true, true]],"
        "'a.b.c': [[1, Infinity, true, true]]}}}}}");
}

// The bounds cannot be compounded. Although there is an $elemMatch over the
// shared path prefix 'a', the predicates must be conjoined by the same $elemMatch,
// without nested $elemMatch's intervening. The bounds could be compounded if
// the query were rewritten as {a: {$elemMatch: {'d.e': {$lte: 1}, 'b.c': {$gte: 1}}}}.
TEST_F(QueryPlannerTest, MultikeyTwoNestedElemMatchBounds) {
    // true means multikey
    addIndex(BSON("a.d.e" << 1 << "a.b.c" << 1), true);
    runQuery(
        fromjson("{a: {$elemMatch: {d: {$elemMatch: {e: {$lte: 1}}},"
                 "b: {$elemMatch: {c: {$gte: 1}}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.d.e': 1, 'a.b.c': 1}, bounds: "
        "{'a.d.e': [[-Infinity, 1, true, true]],"
        "'a.b.c': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

// Bounds can be intersected for a multikey index when the predicates are
// joined by an $elemMatch over the full path of the index field.
TEST_F(QueryPlannerTest, MultikeyElemMatchValue) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(fromjson("{'a.b': {$elemMatch: {$gte: 1, $lte: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[1, 1, true, true]]}}}}}");
}

// We can intersect the bounds for all three predicates because
// the index is not multikey.
TEST_F(QueryPlannerTest, ElemMatchIntersectBoundsNotMultikey) {
    addIndex(BSON("a.b" << 1));
    runQuery(
        fromjson("{a: {$elemMatch: {b: {$elemMatch: {$gte: 1, $lte: 4}}}},"
                 "'a.b': {$in: [2,5]}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[2, 2, true, true]]}}}}}");
}

// Bounds can be intersected for a multikey index when the predicates are
// joined by an $elemMatch over the full path of the index field. The bounds
// from the $in predicate are not intersected with the bounds from the
// remaining to predicates because the $in is not joined to the other
// predicates with an $elemMatch.
TEST_F(QueryPlannerTest, ElemMatchIntersectBoundsMultikey) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(
        fromjson("{a: {$elemMatch: {b: {$elemMatch: {$gte: 1, $lte: 4}}}},"
                 "'a.b': {$in: [2,5]}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[1, 4, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$elemMatch: {$gte: 1, $lte: 4}}}}}, "
        "node: {ixscan: {pattern: {'a.b': 1}, bounds: "
        "{'a.b': [[2,2,true,true], [5,5,true,true]]}}}}}");
}

// Bounds can be intersected because the predicates are joined by an
// $elemMatch over the path "a.b.c", the full path of the multikey
// index field.
TEST_F(QueryPlannerTest, MultikeyNestedElemMatchValue) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {'b.c': {$elemMatch: {$gte: 1, $lte: 1}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1}, bounds: "
        "{'a.b.c': [[1, 1, true, true]]}}}}}");
}

// Bounds cannot be compounded for a multikey compound index when
// the predicates share a prefix (and there is no $elemMatch).
TEST_F(QueryPlannerTest, MultikeySharedPrefixNoElemMatch) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{'a.b': 1, 'a.c': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds can be compounded because there is an $elemMatch applied to the
// shared prefix "a".
TEST_F(QueryPlannerTest, MultikeySharedPrefixElemMatch) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: 1, c: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], 'a.c': [[1,1,true,true]]}}}}}");
}

// Bounds cannot be compounded for the multikey index even though there is an
// $elemMatch, because the $elemMatch does not join the two predicates. This
// query is semantically indentical to {'a.b': 1, 'a.c': 1}.
TEST_F(QueryPlannerTest, MultikeySharedPrefixElemMatchNotShared) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{'a.b': 1, a: {$elemMatch: {c: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds cannot be compounded for the multikey index even though there are
// $elemMatch's, because there is not an $elemMatch which joins the two
// predicates. This query is semantically indentical to {'a.b': 1, 'a.c': 1}.
TEST_F(QueryPlannerTest, MultikeySharedPrefixTwoElemMatches) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{$and: [{a: {$elemMatch: {b: 1}}}, {a: {$elemMatch: {c: 1}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds for the predicates joined by the $elemMatch over the shared prefix
// "a" can be combined. However, the predicate 'a.b'==1 cannot also be combined
// given that it is outside of the $elemMatch.
TEST_F(QueryPlannerTest, MultikeySharedPrefixNoIntersectOutsideElemMatch) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{'a.b': 1, a: {$elemMatch: {b: {$gt: 0}, c: 1}}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[0,Infinity,false,true]], "
        " 'a.c': [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds for the predicates joined by the $elemMatch over the shared prefix
// "a" can be combined. However, the predicate outside the $elemMatch
// cannot also be combined.
TEST_F(QueryPlannerTest, MultikeySharedPrefixNoIntersectOutsideElemMatch2) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: 1, c: 1}}, 'a.b': 1}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds for the predicates joined by the $elemMatch over the shared prefix
// "a" can be combined. However, the predicate outside the $elemMatch
// cannot also be combined.
TEST_F(QueryPlannerTest, MultikeySharedPrefixNoIntersectOutsideElemMatch3) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{'a.c': 2, a: {$elemMatch: {b: 1, c: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1}, bounds: "
        "{'a.b': [[1,1,true,true]], "
        " 'a.c': [[1,1,true,true]]}}}}}");
}

// There are two sets of fields that share a prefix: {'a.b', 'a.c'} and
// {'d.e', 'd.f'}. Since the index is multikey, we can only use the bounds from
// one member of each of these sets.
TEST_F(QueryPlannerTest, MultikeyTwoSharedPrefixesBasic) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1 << "d.e" << 1 << "d.f" << 1), true);
    runQuery(fromjson("{'a.b': 1, 'a.c': 1, 'd.e': 1, 'd.f': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b':1,'a.c':1,'d.e':1,'d.f':1},"
        "bounds: {'a.b':[[1,1,true,true]], "
        " 'a.c':[['MinKey','MaxKey',true,true]], "
        " 'd.e':[[1,1,true,true]], "
        " 'd.f':[['MinKey','MaxKey',true,true]]}}}}}");
}

// All bounds can be combined. Although, 'a.b' and 'a.c' share prefix 'a', the
// relevant predicates are joined by an $elemMatch on 'a'. Similarly, predicates
// over 'd.e' and 'd.f' are joined by an $elemMatch on 'd'.
TEST_F(QueryPlannerTest, MultikeyTwoSharedPrefixesTwoElemMatch) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1 << "d.e" << 1 << "d.f" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: 1, c: 1}}, d: {$elemMatch: {e: 1, f: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 1, c: 1}}},"
        "{d: {$elemMatch: {e: 1, f: 1}}}]},"
        "node: {ixscan: {pattern: {'a.b':1,'a.c':1,'d.e':1,'d.f':1},"
        "bounds: {'a.b':[[1,1,true,true]], "
        " 'a.c':[[1,1,true,true]], "
        " 'd.e':[[1,1,true,true]], "
        " 'd.f':[[1,1,true,true]]}}}}}");
}

// Bounds for 'a.b' and 'a.c' can be combined because of the $elemMatch on 'a'.
// Since predicates an 'd.e' and 'd.f' have no $elemMatch, we use the bounds
// for only one of the two.
TEST_F(QueryPlannerTest, MultikeyTwoSharedPrefixesOneElemMatch) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1 << "d.e" << 1 << "d.f" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: 1, c: 1}}, 'd.e': 1, 'd.f': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{a:{$elemMatch:{b:1,c:1}}}, {'d.f':1}]},"
        "node: {ixscan: {pattern: {'a.b':1,'a.c':1,'d.e':1,'d.f':1},"
        "bounds: {'a.b':[[1,1,true,true]], "
        " 'a.c':[[1,1,true,true]], "
        " 'd.e':[[1,1,true,true]], "
        " 'd.f':[['MinKey','MaxKey',true,true]]}}}}}");
}

// Bounds for 'd.e' and 'd.f' can be combined because of the $elemMatch on 'd'.
// Since predicates an 'a.b' and 'a.c' have no $elemMatch, we use the bounds
// for only one of the two.
TEST_F(QueryPlannerTest, MultikeyTwoSharedPrefixesOneElemMatch2) {
    // true means multikey
    addIndex(BSON("a.b" << 1 << "a.c" << 1 << "d.e" << 1 << "d.f" << 1), true);
    runQuery(fromjson("{'a.b': 1, 'a.c': 1, d: {$elemMatch: {e: 1, f: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:[{d:{$elemMatch:{e:1,f:1}}}, {'a.c':1}]},"
        "node: {ixscan: {pattern: {'a.b':1,'a.c':1,'d.e':1,'d.f':1},"
        "bounds: {'a.b':[[1,1,true,true]], "
        " 'a.c':[['MinKey','MaxKey',true,true]], "
        " 'd.e':[[1,1,true,true]], "
        " 'd.f':[[1,1,true,true]]}}}}}");
}

// The bounds cannot be compounded because 'a.b.x' and 'a.b.y' share prefix
// 'a.b' (and there is no $elemMatch).
TEST_F(QueryPlannerTest, MultikeyDoubleDottedNoElemMatch) {
    // true means multikey
    addIndex(BSON("a.b.x" << 1 << "a.b.y" << 1), true);
    runQuery(fromjson("{'a.b.y': 1, 'a.b.x': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.x':1,'a.b.y':1}, bounds: "
        "{'a.b.x': [[1,1,true,true]], "
        " 'a.b.y': [['MinKey','MaxKey',true,true]]}}}}}");
}

// The bounds can be compounded because the predicates are joined by an
// $elemMatch on the shared prefix "a.b".
TEST_F(QueryPlannerTest, MultikeyDoubleDottedElemMatch) {
    // true means multikey
    addIndex(BSON("a.b.x" << 1 << "a.b.y" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {x: 1, y: 1}}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.x':1,'a.b.y':1}, bounds: "
        "{'a.b.x': [[1,1,true,true]], "
        " 'a.b.y': [[1,1,true,true]]}}}}}");
}

// The bounds cannot be compounded. Although there is an $elemMatch that appears
// to join the predicates, the path to which the $elemMatch is applied is "a".
// Therefore, the predicates contained in the $elemMatch are over "b.x" and "b.y".
// They cannot be compounded due to shared prefix "b".
TEST_F(QueryPlannerTest, MultikeyDoubleDottedUnhelpfulElemMatch) {
    // true means multikey
    addIndex(BSON("a.b.x" << 1 << "a.b.y" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {'b.x': 1, 'b.y': 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.x':1,'a.b.y':1}, bounds: "
        "{'a.b.x': [[1,1,true,true]], "
        " 'a.b.y': [['MinKey','MaxKey',true,true]]}}}}}");
}

// The bounds can be compounded because the predicates are joined by an
// $elemMatch on the shared prefix "a.b".
TEST_F(QueryPlannerTest, MultikeyDoubleDottedElemMatchOnDotted) {
    // true means multikey
    addIndex(BSON("a.b.x" << 1 << "a.b.y" << 1), true);
    runQuery(fromjson("{'a.b': {$elemMatch: {x: 1, y: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.x':1,'a.b.y':1}, bounds: "
        "{'a.b.x': [[1,1,true,true]], "
        " 'a.b.y': [[1,1,true,true]]}}}}}");
}

// This one is subtle. Say we compound the bounds for predicates over "a.b.c" and
// "a.b.d". This is okay because of the predicate over the shared prefix "a.b".
// It might seem like we can do the same for the $elemMatch over shared prefix "a.e",
// thus combining all bounds. But in fact, we can't combine any more bounds because
// we have already used prefix "a". In other words, this query is like having predicates
// over "a.b" and "a.e", so we can only use bounds from one of the two.
TEST_F(QueryPlannerTest, MultikeyComplexDoubleDotted) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1 << "a.e.f" << 1 << "a.b.d" << 1 << "a.e.g" << 1), true);
    runQuery(
        fromjson("{'a.b': {$elemMatch: {c: 1, d: 1}}, "
                 "'a.e': {$elemMatch: {f: 1, g: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c':1,'a.e.f':1,'a.b.d':1,'a.e.g':1},"
        "bounds: {'a.b.c':[[1,1,true,true]], "
        " 'a.e.f':[['MinKey','MaxKey',true,true]], "
        " 'a.b.d':[[1,1,true,true]], "
        " 'a.e.g':[['MinKey','MaxKey',true,true]]}}}}}");
}

// Similar to MultikeyComplexDoubleDotted above.
TEST_F(QueryPlannerTest, MultikeyComplexDoubleDotted2) {
    // true means multikey
    addIndex(BSON("a.b.c" << 1 << "a.e.c" << 1 << "a.b.d" << 1 << "a.e.d" << 1), true);
    runQuery(
        fromjson("{'a.b': {$elemMatch: {c: 1, d: 1}}, "
                 "'a.e': {$elemMatch: {f: 1, g: 1}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c':1,'a.e.c':1,'a.b.d':1,'a.e.d':1},"
        "bounds: {'a.b.c':[[1,1,true,true]], "
        " 'a.e.c':[['MinKey','MaxKey',true,true]], "
        " 'a.b.d':[[1,1,true,true]], "
        " 'a.e.d':[['MinKey','MaxKey',true,true]]}}}}}");
}

// SERVER-13422: check that we plan $elemMatch object correctly with index intersection.
TEST_F(QueryPlannerTest, ElemMatchIndexIntersection) {
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("shortId" << 1));
    // true means multikey
    addIndex(BSON("a.b.startDate" << 1), true);
    addIndex(BSON("a.b.endDate" << 1), true);

    runQuery(
        fromjson("{shortId: 3, 'a.b': {$elemMatch: {startDate: {$lte: 3},"
                 "endDate: {$gt: 6}}}}"));

    assertNumSolutions(6U);

    // 3 single index solutions.
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {shortId: 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.startDate': 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b.endDate': 1}}}}}");

    // 3 index intersection solutions. The last one has to intersect two
    // predicates within the $elemMatch object.
    assertSolutionExists(
        "{fetch: {node: {andHash: {nodes: ["
        "{ixscan: {pattern: {shortId: 1}}},"
        "{ixscan: {pattern: {'a.b.startDate': 1}}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {andHash: {nodes: ["
        "{ixscan: {pattern: {shortId: 1}}},"
        "{ixscan: {pattern: {'a.b.endDate': 1}}}]}}}}");
    assertSolutionExists(
        "{fetch: {node: {andHash: {nodes: ["
        "{ixscan: {pattern: {'a.b.startDate': 1}}},"
        "{ixscan: {pattern: {'a.b.endDate': 1}}}]}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, NegationBelowElemMatchValue) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);

    runQuery(fromjson("{a: {$elemMatch: {$ne: 2}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{$ne:2}}}, node: "
        "{ixscan: {filter: null, pattern: {a: 1}, bounds: "
        "{a: [['MinKey',2,true,false], [2,'MaxKey',false,true]]}}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, AndWithNegationBelowElemMatchValue) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);
    addIndex(BSON("b" << 1), true);

    runQuery(fromjson("{b: 10, a: {$elemMatch: {$not: {$gt: 4}}}}"));

    // One solution using index on 'b' and one using index on 'a'.
    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node: {ixscan: {filter: null, pattern: {b: 1}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}, bounds: {a: "
        "[['MinKey',4,true,true],[Infinity,'MaxKey',false,true]]}}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, AndWithNegationBelowElemMatchValue2) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);

    runQuery(fromjson("{b: 10, a: {$elemMatch: {$not: {$gt: 4}, $gt: 2}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}, bounds: "
        "{a: [[2, 4, false, true]]}}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, NegationBelowElemMatchValueBelowElemMatchObject) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a.b" << 1), true);

    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {$ne: 4}}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {filter: null, pattern: {'a.b': 1}, bounds: "
        "{'a.b': [['MinKey',4,true,false],[4,'MaxKey',false,true]]}}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, NegationBelowElemMatchValueBelowOrBelowAnd) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);
    addIndex(BSON("b" << 1));

    runQuery(fromjson("{c: 3, $or: [{a: {$elemMatch: {$ne: 4, $ne: 3}}}, {b: 5}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {c:3}, node: {or: {nodes: ["
        "{fetch: {node: {ixscan: {filter: null, pattern: {a: 1}, bounds: "
        "{a: [['MinKey',3,true,false],"
        "[3,4,false,false],"
        "[4,'MaxKey',false,true]]}}}}}, "
        "{ixscan: {filter: null, pattern: {b: 1}, bounds: "
        "{b: [[5,5,true,true]]}}}]}}}}");
}

// SERVER-14718
TEST_F(QueryPlannerTest, CantIndexNegationBelowElemMatchValue) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);

    // There are no indexed solutions, because negations of $mod are not indexable.
    runInvalidQuery(fromjson("{a: {$elemMatch: {$not: {$mod: [2, 0]}}}}"));
    assertNoSolutions();
}

/**
 * Index bounds constraints on a field should not be intersected
 * if the index is multikey.
 */
TEST_F(QueryPlannerTest, MultikeyTwoConstraintsSameField) {
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$gt: 0, $lt: 5}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {filter: {$and: [{a: {$lt: 5}}, {a: {$gt: 0}}]}, dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 5}}, node: {ixscan: {filter: null, "
        "pattern: {a: 1}, bounds: {a: [[0, Infinity, false, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$gt: 0}}, node: {ixscan: {filter: null, "
        "pattern: {a: 1}, bounds: {a: [[-Infinity, 5, true, false]]}}}}}");
}

/**
 * Constraints on fields with a shared parent should not be intersected
 * if the index is multikey.
 */
TEST_F(QueryPlannerTest, MultikeyTwoConstraintsDifferentFields) {
    addIndex(BSON("a.b" << 1 << "a.c" << 1), true);
    runQuery(fromjson("{'a.b': 2, 'a.c': 3}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {filter: {$and: [{'a.b': 2}, {'a.c': 3}]}, dir: 1}}");

    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {'a.c': 3}, node: {ixscan: {filter: null, "
        "pattern: {'a.b': 1, 'a.c': 1}, bounds: "
        "{'a.b': [[2,2,true,true]], "
        " 'a.c': [['MinKey','MaxKey',true,true]]}}}}}");
    alternates.push_back(
        "{fetch: {filter: {'a.b': 2}, node: {ixscan: {filter: null, "
        "pattern: {'a.b': 1, 'a.c': 1}, bounds: "
        "{'a.b': [['MinKey','MaxKey',true,true]], "
        " 'a.c': [[3,3,true,true]]}}}}}");
    assertHasOneSolutionOf(alternates);
}

// SERVER-16042
TEST_F(QueryPlannerTest, MultikeyElemMatchAll) {
    addIndex(BSON("a.b" << 1), true);
    runQuery(fromjson("{a: {$all: [{$elemMatch: {b: {$gt: 1}}}, {$elemMatch: {b: {$lt: 0}}}]}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}}]},"
        "node: {ixscan: {pattern: {'a.b': 1}, filter: null,"
        "bounds: {'a.b': [[-Infinity, 0, true, false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}}]},"
        "node: {ixscan: {pattern: {'a.b': 1}, filter: null,"
        "bounds: {'a.b': [[1, Infinity, false, true]]}}}}}");
}

// SERVER-16042
TEST_F(QueryPlannerTest, MultikeyElemMatchAllCompound) {
    addIndex(BSON("a.b" << 1 << "c" << 1), true);
    runQuery(
        fromjson("{a: {$all: [{$elemMatch: {b: {$gt: 1}}}, "
                 "{$elemMatch: {b: {$lt: 0}}}]}, c: 3}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}}]},"
        "node: {ixscan: {pattern: {'a.b': 1, c: 1}, filter: null,"
        "bounds: {'a.b': [[-Infinity,0,true,false]], c: [[3,3,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}}]},"
        "node: {ixscan: {pattern: {'a.b': 1, c: 1}, filter: null,"
        "bounds: {'a.b': [[1,Infinity,false,true]], c: [[3,3,true,true]]}}}}}");
}

// SERVER-16042
TEST_F(QueryPlannerTest, MultikeyElemMatchAllCompound2) {
    addIndex(BSON("a.b" << 1 << "c" << 1), true);
    runQuery(
        fromjson("{a: {$all: [{$elemMatch: {b: {$gt: 1}}}, "
                 "{$elemMatch: {b: {$lt: 0}}}]}, c: {$gte: 3, $lte: 4}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:"
        "[{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}},{c:{$gte:3}}]},"
        "node: {ixscan: {pattern: {'a.b': 1, c: 1}, filter: null,"
        "bounds: {'a.b': [[-Infinity,0,true,false]], c: [[-Infinity,4,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and:"
        "[{a:{$elemMatch:{b:{$lt:0}}}},{a:{$elemMatch:{b:{$gt:1}}}},{c:{$gte:3}}]},"
        "node: {ixscan: {pattern: {'a.b': 1, c: 1}, filter: null,"
        "bounds: {'a.b': [[1,Infinity,false,true]], c: [[-Infinity,4,true,true]]}}}}}");
}

// SERVER-16042
TEST_F(QueryPlannerTest, MultikeyElemMatchAllCompound3) {
    addIndex(BSON("arr.k" << 1 << "arr.v" << 1), true);
    runQuery(fromjson(
        "{arr: {$all: ["
        "{$elemMatch: {k: 1, v: 1}}, {$elemMatch: {k: 2, v: 2}}, {$elemMatch: {k: 3, v: 3}}]}}"));

    assertNumSolutions(4U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: "
        "{$and:[{arr:{$elemMatch:{k:1,v:1}}},"
        "{arr:{$elemMatch:{k:2,v:2}}},{arr:{$elemMatch:{k:3,v:3}}}]},"
        "node: {ixscan: {pattern: {'arr.k': 1, 'arr.v': 1}, filter: null, "
        "bounds: {'arr.k': [[1,1,true,true]], 'arr.v': [[1,1,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: "
        "{$and:[{arr:{$elemMatch:{k:2,v:2}}},"
        "{arr:{$elemMatch:{k:1,v:1}}},{arr:{$elemMatch:{k:3,v:3}}}]},"
        "node: {ixscan: {pattern: {'arr.k': 1, 'arr.v': 1}, filter: null, "
        "bounds: {'arr.k': [[2,2,true,true]], 'arr.v': [[2,2,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: "
        "{$and:[{arr:{$elemMatch:{k:3,v:3}}},"
        "{arr:{$elemMatch:{k:1,v:1}}},{arr:{$elemMatch:{k:2,v:2}}}]},"
        "node: {ixscan: {pattern: {'arr.k': 1, 'arr.v': 1}, filter: null, "
        "bounds: {'arr.k': [[3,3,true,true]], 'arr.v': [[3,3,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenFirstFieldIsNotMultikey) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, 10, true, false]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnFirstFieldWhenItAndSharedPrefixAreNotMultikey) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}, {1U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[0, 10, true, false]], 'a.c': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsWhenFirstFieldIsMultikey) {
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 10}}, node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, Infinity, true, true]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 0}}, node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[-Infinity, 10, true, false]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenFirstFieldIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {$gte: 0, $lt: 10}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$elemMatch: {$gte: 0, $lt: 10}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, 10, true, false]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanComplementBoundsOnFirstFieldWhenItIsMultikeyAndHasNotEqualExpr) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$ne: 3}, b: 2}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [['MinKey', 3, true, false], [3, 'MaxKey', false, true]], "
        "b: [[2, 2, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenFirstFieldIsMultikeyAndHasNotInsideElemMatch) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {$not: {$gte: 10}, $gte: 0}}, b: 2}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, 10, true, false]], b: [[2, 2, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnFirstFieldWhenSharedPrefixIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: {$gte: 0, $lt: 10}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$elemMatch: {b: {$gte: 0, $lt: 10}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[0, 10, true, false]], 'a.c': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsOnFirstFieldWhenItAndSharedPrefixAreMultikey) {
    MultikeyPaths multikeyPaths{{0U, 1U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: {$gte: 0, $lt: 10}, c: 2}}}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: {a: {$elemMatch: {b: {$gte: 0, $lt: 10}, c: 2}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[0, Infinity, true, true]], 'a.c': [[2, 2, true, true]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[-Infinity, 10, true, false]], 'a.c': [[2, 2, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenSecondFieldIsNotMultikey) {
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnSecondFieldWhenItAndSharedPrefixAreNotMultikey) {
    MultikeyPaths multikeyPaths{{1U}, MultikeyComponents{}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': 2, 'a.c': {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': 2, 'a.c': {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsWhenSecondFieldIsMultikey) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[-Infinity, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenSecondFieldIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$elemMatch: {$gte: 0, $lt: 10}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$elemMatch: {$gte: 0, $lt: 10}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanComplementBoundsOnSecondFieldWhenItIsMultikeyAndHasNotEqualExpr) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$ne: 3}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], "
        "b: [['MinKey', 3, true, false], [3, 'MaxKey', false, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenSecondFieldIsMultikeyAndHasNotInsideElemMatch) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$elemMatch: {$not: {$gte: 10}, $gte: 0}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnSecondFieldWhenSharedPrefixIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: 2, c: {$gte: 0, $lt: 10}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: {a: {$elemMatch: {b: 2, c: {$gte: 0, $lt: 10}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsOnSecondFieldWhenItAndSharedPrefixAreMultikey) {
    MultikeyPaths multikeyPaths{{0U}, {0U, 1U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: 2, c: {$gte: 0, $lt: 10}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{cscan: {dir: 1, filter: {a: {$elemMatch: {b: 2, c: {$gte: 0, $lt: 10}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[-Infinity, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsOfTwoSeparateElemMatches) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);

    runQuery(
        fromjson("{$and: [{a: {$elemMatch: {b: {$gte: 0}, c: {$lt: 20}}}}, "
                 "{a: {$elemMatch: {b: {$lt: 10}, c: {$gte: 5}}}}]}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{cscan: {dir: 1, "
        "filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}, c: {$lt: 20}}}}, "
        "{a: {$elemMatch: {b: {$lt: 10}, c: {$gte: 5}}}}]}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[0, Infinity, true, true]], 'a.c': [[-Infinity, 20, true, false]]}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[-Infinity, 10, true, false]], 'a.c': [[5, Infinity, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanCompoundBoundsWhenSharedPrefixIsNotMultikey) {
    MultikeyPaths multikeyPaths{{1U}, {1U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': 2, 'a.c': 3}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': 2, 'a.c': 3}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[3, 3, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotCompoundBoundsWhenSharedPrefixIsMultikey) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': 2, 'a.c': 3}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': 2, 'a.c': 3}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanCompoundBoundsWhenSharedPrefixIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: 2, c: 3}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$elemMatch: {b: 2, c: 3}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[3, 3, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotCompoundBoundsWhenSharedPrefixInsideElemMatchIsMultikey) {
    MultikeyPaths multikeyPaths{{0U, 1U}, {0U, 1U}};
    addIndex(BSON("a.b.c" << 1 << "a.b.d" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {'b.c': 2, 'b.d': 3}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$elemMatch: {'b.c': 2, 'b.d': 3}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b.c': 1, 'a.b.d': 1}, "
        "bounds: {'a.b.c': [[2, 2, true, true]], 'a.b.d': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanMakeCoveredPlanForNonArrayLeadingFieldWithPathLevelMultikeyInfo) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 2}, projection: {_id: 0, a: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {ixscan: {pattern: {a: 1, b: 1},"
        "filter: null, bounds: {a: [[1,1,true,true]], b: [[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanMakeCoveredPlanForNonArrayTrailingFieldWithPathLevelMultikeyInfo) {
    MultikeyPaths multikeyPaths{{1U}, {}, {1U}};
    addIndex(BSON("a.z" << 1 << "b" << 1 << "c.z" << 1), multikeyPaths);
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {'a.z': 1, 'c.z': 2, b: 3}, projection: {_id: 0, b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, b: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id:0,b:1}, node: {ixscan: {pattern: {'a.z':1,b:1,'c.z':1}, filter: null,"
        "bounds: {'a.z':[[1,1,true,true]],b:[[3,3,true,true]],'c.z':[[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanCoverNonMultikeyDottedField) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a.y" << 1 << "b.z" << 1), multikeyPaths);
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {'a.y': 1, 'b.z': 2}, projection: {_id: 0, 'a.y': 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id:0,'a.y':1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id:0,'a.y':1}, node: {ixscan:"
        "{pattern: {'a.y':1,'b.z':1}, filter: null,"
        "bounds: {'a.y':[[1,1,true,true]],'b.z':[[2,2,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, ContainedOrElemMatchValue) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: {$elemMatch: {$eq: 5}}}, {$or: [{b: 6}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{or: {nodes: ["
        "{fetch: {filter: {a: {$elemMatch: {$eq: 5}}}, node: {ixscan: {pattern: {b: 1, a: 1}, "
        "bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, true]]}}}}},"
        "{fetch: {filter: {a: {$elemMatch: {$eq: 5}}}, node: {ixscan: {pattern: {c: 1, a: 1}, "
        "bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, true]]}}}}}"
        "]}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrElemMatchObject) {
    addIndex(BSON("c" << 1 << "a.b" << 1));
    addIndex(BSON("d" << 1 << "a.b" << 1));

    runQuery(fromjson("{$and: [{a: {$elemMatch: {b: 5}}}, {$or: [{c: 6}, {d: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: 5}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {c: 1, 'a.b': 1}, bounds: {c: [[6, 6, true, true]], 'a.b': [[5, 5, "
        "true, true]]}}},"
        "{ixscan: {pattern: {d: 1, 'a.b': 1}, bounds: {d: [[7, 7, true, true]], 'a.b': [[5, 5, "
        "true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrElemMatchObjectMultiplePredicates) {
    addIndex(BSON("d" << 1 << "a.b" << 1));
    addIndex(BSON("e" << 1 << "a.c" << 1));

    runQuery(fromjson("{$and: [{a: {$elemMatch: {b: 5, c: 6}}}, {$or: [{d: 7}, {e: 8}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: 5, c: 6}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.b': 1}, bounds: {d: [[7, 7, true, true]], 'a.b': [[5, 5, "
        "true, true]]}}},"
        "{ixscan: {pattern: {e: 1, 'a.c': 1}, bounds: {e: [[8, 8, true, true]], 'a.c': [[6, 6, "
        "true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrNestedElemMatchObject) {
    addIndex(BSON("d" << 1 << "a.b.c" << 1));
    addIndex(BSON("e" << 1 << "a.b.c" << 1));

    runQuery(fromjson(
        "{$and: [{a: {$elemMatch: {b: {$elemMatch: {c: 5}}}}}, {$or: [{d: 6}, {e: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$elemMatch: {c: 5}}}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.b.c': 1}, bounds: {d: [[6, 6, true, true]], 'a.b.c': [[5, 5, "
        "true, true]]}}},"
        "{ixscan: {pattern: {e: 1, 'a.b.c': 1}, bounds: {e: [[7, 7, true, true]], 'a.b.c': [[5, 5, "
        "true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveToElemMatchValue) {
    addIndex(BSON("b" << 1 << "a" << 1));
    addIndex(BSON("c" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: {$elemMatch: {$eq: 6}}}, {c: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {b: {$elemMatch: {$eq: 6}}}, node: {ixscan: {pattern: {b: 1, a: 1}, "
        "bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, true]]}}}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveToElemMatchObject) {
    addIndex(BSON("b.c" << 1 << "a" << 1));
    addIndex(BSON("d" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: {$elemMatch: {c: 6}}}, {d: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {b: {$elemMatch: {c: 6}}}, node: {ixscan: {pattern: {'b.c': 1, a: 1}, "
        "bounds: {'b.c': [[6, 6, true, true]], a: [[5, 5, true, true]]}}}}},"
        "{ixscan: {pattern: {d: 1, a: 1}, bounds: {d: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveToElemMatchObjectMultiplePredicates) {
    addIndex(BSON("b.c" << 1 << "a" << 1 << "b.d" << 1));
    addIndex(BSON("e" << 1 << "a" << 1));

    runQuery(fromjson("{$and: [{a: 5}, {$or: [{b: {$elemMatch: {c: 6, d: 7}}}, {e: 8}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {b: {$elemMatch: {c: 6, d: 7}}}, node: {ixscan: {pattern: {'b.c': 1, a: "
        "1, 'b.d': 1}, bounds: {'b.c': [[6, 6, true, true]], a: [[5, 5, true, true]], 'b.d': [[7, "
        "7, true, true]]}}}}},"
        "{ixscan: {pattern: {e: 1, a: 1}, bounds: {e: [[8, 8, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveToNestedElemMatchObject) {
    addIndex(BSON("b.c.d" << 1 << "a" << 1));
    addIndex(BSON("e" << 1 << "a" << 1));

    runQuery(fromjson(
        "{$and: [{a: 5}, {$or: [{b: {$elemMatch: {c: {$elemMatch: {d: 6}}}}}, {e: 7}]}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{fetch: {filter: {b: {$elemMatch: {c: {$elemMatch: {d: 6}}}}}, node: {ixscan: {pattern: "
        "{'b.c.d': 1, a: 1}, bounds: {'b.c.d': [[6, 6, true, true]], a: [[5, 5, true, true]]}}}}},"
        "{ixscan: {pattern: {e: 1, a: 1}, bounds: {e: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrInElemMatch) {
    addIndex(BSON("a.c" << 1 << "a.b" << 1));
    addIndex(BSON("a.d" << 1 << "a.b" << 1));

    runQuery(fromjson("{a: {$elemMatch: {$and: [{b: 5}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{$or: [{$and: [{c: 6}, {b: 5}]}, {$and: [{d: "
        "7}, {b: 5}]}]}]}}}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1, 'a.b': 1}, bounds: {'a.d': [[7, 7, true, true]], 'a.b': "
        "[[5, 5, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrInAndInElemMatch) {
    addIndex(BSON("b.d" << 1 << "b.c" << 1));
    addIndex(BSON("b.e" << 1 << "b.c" << 1));

    runQuery(
        fromjson("{$and: [{a: 5}, {b: {$elemMatch: {$and: [{c: 5}, {$or: [{d: 6}, {e: 7}]}]}}}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: 5}, {b: {$elemMatch: {$and: [{c: 5}, {$or: [{$and: [{d: 6}, "
        "{c: 5}]}, {$and: [{e: 7}, {c: 5}]}]}]}}}]}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'b.d': 1, 'b.c': 1}, bounds: {'b.d': [[6, 6, true, true]], 'b.c': "
        "[[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {'b.e': 1, 'b.c': 1}, bounds: {'b.e': [[7, 7, true, true]], 'b.c': "
        "[[5, 5, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrElemMatchPredicateIsLeadingFieldIndexIntersection) {
    params.mainCollectionInfo.options =
        QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
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
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCombineLeadingFields) {
    const bool multikey = true;
    addIndex(BSON("a.b" << 1), multikey);
    addIndex(BSON("a.c" << 1), multikey);

    runQuery(
        fromjson("{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{b: {$lte: 10}}, {c: 6}]}]}}}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{b: {$lte: 10}}, {c: "
        "6}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {'a.c': 1}, bounds: {'a.c': [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{b: {$lte: 10}}, {c: "
        "6}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCombineLeadingFields) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("a.b" << 1), multikeyPaths);
    addIndex(BSON("a.c" << 1), multikeyPaths);

    runQuery(
        fromjson("{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{b: {$lte: 10}}, {c: 6}]}]}}}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{$and: [{b: {$lte: 10}}, "
        "{b: {$gte: 0}}]}, {c: 6}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[0, 10, true, true]]}}},"
        "{ixscan: {pattern: {'a.c': 1}, bounds: {'a.c': [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {$or: [{b: {$lte: 10}}, {c: "
        "6}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCombineLeadingFields) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("a.b" << 1), multikeyPaths);
    addIndex(BSON("a.c" << 1), multikeyPaths);

    runQuery(
        fromjson("{$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {$or: [{b: {$lte: "
                 "10}}, {c: 6}]}}}]}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {$or: [{b: "
        "{$lte: 10}}, {c: 6}]}}}]}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {'a.c': 1}, bounds: {'a.c': [[6, 6, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {$or: [{b: "
        "{$lte: 10}}, {c: 6}]}}}]}, node: "
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[0, Infinity, true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCompoundFields) {
    const bool multikey = true;
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikey);
    addIndex(BSON("a.d" << 1), multikey);

    runQuery(fromjson("{a: {$elemMatch: {$and: [{b: 5}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {$or: [{$and: [{b: 5}, {c: 6}]}, {d: "
        "7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {$or: [{c: 6}, {d: 7}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCompoundFields) {
    const bool multikey = true;
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikey);
    addIndex(BSON("a.d" << 1), multikey);

    runQuery(fromjson(
        "{$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {$or: [{c: 6}, {d: 7}]}}}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {$or: [{c: 6}, {d: "
        "7}]}}}]}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCompoundFields) {
    MultikeyPaths multikeyPaths1{{0U}, {0U}};
    MultikeyPaths multikeyPaths2{{0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths1);
    addIndex(BSON("a.d" << 1), multikeyPaths2);

    runQuery(fromjson("{a: {$elemMatch: {$and: [{b: 5}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {$or: [{$and: [{b: 5}, {c: 6}]}, {d: "
        "7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {$or: [{c: 6}, {d: 7}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCompoundFields) {
    MultikeyPaths multikeyPaths1{{0U}, {0U}};
    MultikeyPaths multikeyPaths2{{0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths1);
    addIndex(BSON("a.d" << 1), multikeyPaths2);

    runQuery(fromjson(
        "{$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {$or: [{c: 6}, {d: 7}]}}}]}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {$or: [{c: 6}, {d: "
        "7}]}}}]}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[5, 5, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCombineLeadingOutsidePreds) {
    const bool multikey = true;
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikey);
    addIndex(BSON("a.d" << 1), multikey);

    runQuery(fromjson(
        "{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(5);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$lte: 10}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[-Infinity, 10, true, true]], "
        "'a.c': [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$gte: 0}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[0, Infinity, true, true]], "
        "'a.c': [[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, "
        "{d: 7}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[-Infinity, 10, true, true]], "
        "'a.c': [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, "
        "{d: 7}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[0, Infinity, true, true]], "
        "'a.c': [['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCombineLeadingOutsidePreds) {
    MultikeyPaths multikeyPaths1{{0U}, {0U}};
    MultikeyPaths multikeyPaths2{{0U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths1);
    addIndex(BSON("a.d" << 1), multikeyPaths2);

    runQuery(fromjson(
        "{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(3);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$lte: 10}}, {b: {$gte: 0}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[0, 10, true, true]], 'a.c': "
        "[[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, "
        "{d: 7}]}]}}}, node: "
        "{ixscan: {pattern: {'a.b': 1, 'a.c': 1}, bounds: {'a.b': [[0, 10, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCombineLeadingOutsidePreds) {
    MultikeyPaths multikeyPaths{{0U}, {}};
    addIndex(BSON("a.b" << 1 << "c" << 1), multikeyPaths);
    addIndex(BSON("d" << 1));

    runQuery(
        fromjson("{$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: 10}}}}, "
                 "{$or: [{c: 6}, {d: 7}]}]}"));
    assertNumSolutions(5);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}]}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, c: 1}, bounds: {'a.b': [[-Infinity, 10, true, true]], c: "
        "[[6, 6, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}]}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1, c: 1}, bounds: {'a.b': [[0, Infinity, true, true]], c: [[6, "
        "6, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}, {$or: [{c: 6}, {d: 7}]}]}, node: "
        "{ixscan: {pattern: {'a.b': 1, c: 1}, bounds: {'a.b': [[-Infinity, 10, true, true]], c: "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}, {$or: [{c: 6}, {d: 7}]}]}, node: "
        "{ixscan: {pattern: {'a.b': 1, c: 1}, bounds: {'a.b': [[0, Infinity, true, true]], c: "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCombineTrailingOutsidePreds) {
    const bool multikey = true;
    addIndex(BSON("a.c" << 1 << "a.b" << 1), multikey);
    addIndex(BSON("a.d" << 1), multikey);

    runQuery(fromjson(
        "{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$gte: 0}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[[0, Infinity, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$lte: 10}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCombineTrailingOutsidePreds) {
    MultikeyPaths multikeyPaths1{{0U}, {0U}};
    MultikeyPaths multikeyPaths2{{0U}};
    addIndex(BSON("a.c" << 1 << "a.b" << 1), multikeyPaths1);
    addIndex(BSON("a.d" << 1), multikeyPaths2);

    runQuery(fromjson(
        "{a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{c: 6}, {d: 7}]}]}}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: {$gte: 0}}, {b: {$lte: 10}}, {$or: [{$and: "
        "[{b: {$gte: 0}}, {b: {$lte: 10}}, {c: 6}]}, {d: 7}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.c': 1, 'a.b': 1}, bounds: {'a.c': [[6, 6, true, true]], 'a.b': "
        "[[0, 10, true, true]]}}},"
        "{ixscan: {pattern: {'a.d': 1}, bounds: {'a.d': [[7, 7, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCombineTrailingOutsidePreds) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("c" << 1 << "a.b" << 1), multikeyPaths);
    addIndex(BSON("d" << 1));

    runQuery(
        fromjson("{$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: 10}}}}, "
                 "{$or: [{c: 6}, {d: 7}]}]}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}]}, node: {or: {nodes: ["
        "{ixscan: {pattern: {c: 1, 'a.b': 1}, bounds: {c: [[6, 6, true, true]], 'a.b': [[0, "
        "Infinity, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: {$gte: 0}}}}, {a: {$elemMatch: {b: {$lte: "
        "10}}}}]}, node: {or: {nodes: ["
        "{ixscan: {pattern: {c: 1, 'a.b': 1}, bounds: {c: [[6, 6, true, true]], 'a.b': "
        "[[-Infinity, 10, true, true]]}}},"
        "{ixscan: {pattern: {d: 1}, bounds: {d: [[7, 7, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCompoundTrailingOutsidePreds) {
    const bool multikey = true;
    addIndex(BSON("a.d" << 1 << "a.c" << 1 << "a.b" << 1), multikey);
    addIndex(BSON("a.e" << 1), multikey);

    runQuery(fromjson("{a: {$elemMatch: {$and: [{b: 5}, {c: 6}, {$or: [{d: 7}, {e: 8}]}]}}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {c: 6}, {$or: [{$and: [{b: 5}, {c: 6}, "
        "{d: 7}]}, {e: 8}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.d': 1, 'a.c': 1, 'a.b': 1}, bounds: {'a.d': [[7, 7, true, true]], "
        "'a.c': [[6, 6, true, true]], 'a.b': [[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {'a.e': 1}, bounds: {'a.e': [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMultikeyCannotCompoundTrailingOutsidePreds) {
    const bool multikey = true;
    addIndex(BSON("d" << 1 << "a.c" << 1 << "a.b" << 1), multikey);
    addIndex(BSON("e" << 1));

    runQuery(fromjson(
        "{$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {c: 6}}}, {$or: [{d: 7}, {e: 8}]}]}"));
    assertNumSolutions(2);
    std::vector<std::string> alternates;
    alternates.push_back(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {c: 6}}}]}, node: "
        "{or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.c': 1, 'a.b': 1}, bounds: {d: [[7, 7, true, true]], 'a.c': "
        "[[6, 6, true, true]], 'a.b': [['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    alternates.push_back(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {c: 6}}}]}, node: "
        "{or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.c': 1, 'a.b': 1}, bounds: {d: [[7, 7, true, true]], 'a.c': "
        "[['MinKey', 'MaxKey', true, true]], 'a.b': [[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertHasOneSolutionOf(alternates);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCompoundTrailingOutsidePreds) {
    MultikeyPaths multikeyPaths1{{0U}, {0U}, {0U}};
    MultikeyPaths multikeyPaths2{{0U}};
    addIndex(BSON("a.d" << 1 << "a.c" << 1 << "a.b" << 1), multikeyPaths1);
    addIndex(BSON("a.e" << 1), multikeyPaths2);

    runQuery(fromjson("{a: {$elemMatch: {$and: [{b: 5}, {c: 6}, {$or: [{d: 7}, {e: 8}]}]}}}"));
    assertNumSolutions(2);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$and: [{b: 5}, {c: 6}, {$or: [{$and: [{b: 5}, {c: 6}, "
        "{d: 7}]}, {e: 8}]}]}}}, node: {or: {nodes: ["
        "{ixscan: {pattern: {'a.d': 1, 'a.c': 1, 'a.b': 1}, bounds: {'a.d': [[7, 7, true, true]], "
        "'a.c': [[6, 6, true, true]], 'a.b': [[5, 5, true, true]]}}},"
        "{ixscan: {pattern: {'a.e': 1}, bounds: {'a.e': [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrPathLevelMultikeyCannotCompoundTrailingOutsidePreds) {
    MultikeyPaths multikeyPaths{{}, {0U}, {0U}};
    addIndex(BSON("d" << 1 << "a.c" << 1 << "a.b" << 1), multikeyPaths);
    addIndex(BSON("e" << 1));

    runQuery(fromjson(
        "{$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {c: 6}}}, {$or: [{d: 7}, {e: 8}]}]}"));
    assertNumSolutions(2);
    // When we have path-level multikey info, we ensure that predicates are assigned in order of
    // index position.
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: {$elemMatch: {b: 5}}}, {a: {$elemMatch: {c: 6}}}]}, node: "
        "{or: {nodes: ["
        "{ixscan: {pattern: {d: 1, 'a.c': 1, 'a.b': 1}, bounds: {d: [[7, 7, true, true]], 'a.c': "
        "[[6, 6, true, true]], 'a.b': [['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {e: 1}, bounds: {e: [[8, 8, true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrCannotPushdownThroughElemMatchObj) {
    addIndex(BSON("a" << 1 << "b.c" << 1));

    runQuery(fromjson("{a: 1, b: {$elemMatch: {$or: [{c: 2}, {c: 3}]}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$elemMatch: {c: {$in: [2, 3]}}}},"
        "node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}, "
        "bounds: {a: [[1,1,true,true]], 'b.c': [[2,2,true,true],[3,3,true,true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrCannotPushdownThroughElemMatchObjWithMultikeyPaths) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "b.c" << 1), multikeyPaths);

    runQuery(fromjson("{a: 1, b: {$elemMatch: {$or: [{c: 2}, {c: 3}]}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$elemMatch: {c: {$in: [2, 3]}}}},"
        "node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}, "
        "bounds: {a: [[1,1,true,true]], 'b.c': [[2,2,true,true],[3,3,true,true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrCannotPushdownThroughOrElemMatchObjOrPattern) {
    addIndex(BSON("a" << 1 << "b.c" << 1));

    runQuery(fromjson("{a: 1, $or: [{a: 2}, {b: {$elemMatch: {$or: [{c: 3}, {c: 4}]}}}]}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: 2}, {b: {$elemMatch: {c: {$in: [3, 4]}}}}]},"
        "node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}, "
        "bounds: {a: [[1,1,true,true]], 'b.c': [['MinKey','MaxKey',true,true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrCannotPushdownThroughOrElemMatchObjOrPatternWithMultikeyPaths) {
    MultikeyPaths multikeyPaths{{}, {0U}};
    addIndex(BSON("a" << 1 << "b.c" << 1), multikeyPaths);

    runQuery(fromjson("{a: 1, $or: [{a: 2}, {b: {$elemMatch: {$or: [{c: 3}, {c: 4}]}}}]}"));

    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {$or: [{a: 2}, {b: {$elemMatch: {c: {$in: [3, 4]}}}}]},"
        "node: "
        "{ixscan: {filter: null, pattern: {a: 1, 'b.c': 1}, "
        "bounds: {a: [[1,1,true,true]], 'b.c': [['MinKey','MaxKey',true,true]]}}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

// TODO SERVER-30145: Fixing this ticket should allow us to generate tight bounds on "b.c.f" below.
TEST_F(QueryPlannerTest, ContainedOrInAndInNestedElemMatch) {
    addIndex(BSON("b.d" << 1 << "b.c.f" << 1));
    addIndex(BSON("b.e" << 1 << "b.c.f" << 1));

    runQuery(
        fromjson("{$and: [{a: 5}, {b: {$elemMatch: {$and: ["
                 "{c: {$elemMatch: {f: 5}}}, {$or: [{d: 6}, {e: 7}]}]}}}]}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: 5}, {b: {$elemMatch: {$and: [{c: {$elemMatch: {f: 5}}}, "
        "{$or: [{d: 6}, {e: 7}]}]}}}]}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'b.d': 1, 'b.c.f': 1}, bounds: {'b.d': [[6, 6, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {'b.e': 1, 'b.c.f': 1}, bounds: {'b.e': [[7, 7, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

// TODO SERVER-30145: Fixing this ticket should allow us to generate tight bounds on "b.c.f" below.
TEST_F(QueryPlannerTest, ContainedOrInAndInNestedElemMatchWithMultikeyPaths) {
    MultikeyPaths multikeyPaths{{0U}, {0U, 1U}};
    addIndex(BSON("b.d" << 1 << "b.c.f" << 1), multikeyPaths);
    addIndex(BSON("b.e" << 1 << "b.c.f" << 1), multikeyPaths);

    runQuery(
        fromjson("{$and: [{a: 5}, {b: {$elemMatch: {$and: ["
                 "{c: {$elemMatch: {f: 5}}}, {$or: [{d: 6}, {e: 7}]}]}}}]}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {$and: [{a: 5}, {b: {$elemMatch: {$and: [{c: {$elemMatch: {f: 5}}}, "
        "{$or: [{d: 6}, {e: 7}]}]}}}]}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'b.d': 1, 'b.c.f': 1}, bounds: {'b.d': [[6, 6, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {'b.e': 1, 'b.c.f': 1}, bounds: {'b.e': [[7, 7, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

// TODO SERVER-30145: Fixing this ticket should allow us to generate tight bounds on "b.c.f" below.
TEST_F(QueryPlannerTest, ContainedOrInNestedElemMatchWithMultikeyPaths) {
    MultikeyPaths multikeyPaths{{0U}, {0U, 1U}};
    addIndex(BSON("b.d" << 1 << "b.c.f" << 1), multikeyPaths);
    addIndex(BSON("b.e" << 1 << "b.c.f" << 1), multikeyPaths);

    runQuery(fromjson("{b: {$elemMatch: {c: {$elemMatch: {f: 5}}, $or: [{d: 6}, {e: 7}]}}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$elemMatch: {c: {$elemMatch: {f: 5}}, $or: [{d: 6}, {e: 7}]}}}, "
        "node: {or: {nodes: ["
        "{ixscan: {pattern: {'b.d': 1, 'b.c.f': 1}, bounds: {'b.d': [[6, 6, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}},"
        "{ixscan: {pattern: {'b.e': 1, 'b.c.f': 1}, bounds: {'b.e': [[7, 7, true, true]], 'b.c.f': "
        "[['MinKey', 'MaxKey', true, true]]}}}"
        "]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ContainedOrMoveElemMatchToNestedElemMatchObject) {
    addIndex(BSON("b.c.d" << 1 << "a.f" << 1), MultikeyPaths{{0U, 1U}, {0U}});
    addIndex(BSON("e" << 1 << "a.f" << 1), MultikeyPaths{{}, {0U}});

    runQuery(fromjson(
        "{a: {$elemMatch: {f: 5}}, $or: [{b: {$elemMatch: {c: {$elemMatch: {d: 6}}}}}, {e: 7}]}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {f: 5}}}, node: {or: {nodes: ["
        "{fetch: {filter: {b: {$elemMatch: {c: {$elemMatch: {d: 6}}}}}, node: {ixscan: {pattern: "
        "{'b.c.d': 1, 'a.f': 1}, bounds: {'b.c.d': [[6, 6, true, true]], 'a.f': [[5, 5, true, "
        "true]]}}}}},"
        "{ixscan: {pattern: {e: 1, 'a.f': 1}, bounds: {e: [[7, 7, true, true]], 'a.f': [[5, 5, "
        "true, true]]}}}]}}}}");
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, TypeArrayUsingTypeCodeMustFetchAndFilter) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$type: 4}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$type: 'array'}}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, TypeArrayUsingStringAliasMustFetchAndFilter) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$type: 'array'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$type: 'array'}}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CantExplodeMultikeyIxscanForSort) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    const bool multikey = true;
    addIndex(BSON("a" << 1 << "b" << 1), multikey);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {$in: [1, 2]}}, sort: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node:"
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, b: 1}, filter: null, bounds: {a: "
        "[[1,1,true,true], [2,2,true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, CantExplodeMultikeyIxscanForSortWithPathLevelMultikeyMetadata) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{MultikeyComponents{}, {0U}};
    addIndex(BSON("a" << 1 << "b.c" << 1), multikeyPaths);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {$in: [1, 2]}}, sort: {'b.c': 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {'b.c': 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, 'b.c': 1}, filter: null, bounds: "
        "{a: [[1,1,true,true], [2,2,true,true]], 'b.c': [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, CanExplodeMultikeyIndexScanForSortWhenSortFieldsAreNotMultikey) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, MultikeyComponents{}};
    addIndex(BSON("a" << 1 << "b.c" << 1), multikeyPaths);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {$in: [1, 2]}}, sort: {'b.c': 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan: {pattern: {a: 1, 'b.c': 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], 'b.c': [['MinKey','MaxKey',true,true]]}}},"
        "{ixscan: {pattern: {a: 1, 'b.c': 1}, filter: null,"
        "bounds: {a: [[2,2,true,true]], 'b.c': [['MinKey','MaxKey',true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, CanExploreMultikeyIndexScanForSortWhenFieldsNotUsedForSortAreMultikey) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    // Index {a: 1, b: 1, c: 1} where 'c' is multikey
    MultikeyPaths multikeyPaths{{}, {}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1 << "c" << 1), multikeyPaths);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {$in: [1, 2]}}, sort: {'b': 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, filter: null,"
        "bounds: {a: [[1,1,true,true]], b: [['MinKey','MaxKey',true,true]], "
        "c: [['MinKey','MaxKey',true,true]]}}},"
        "{ixscan: {pattern: {a: 1, b: 1, c: 1}, filter: null,"
        "bounds: {a: [[2,2,true,true]], b: [['MinKey','MaxKey',true,true]], "
        "c: [['MinKey','MaxKey',true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, MultikeyIndexScanWithMinKeyMaxKeyBoundsCanProvideSort) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("a" << 1), multikeyPaths);
    runQueryAsCommand(fromjson("{find: 'testns', sort: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1}, bounds: {a: "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, MultikeyIndexScanWithBoundsOnIndexWithoutSharedPrefixCanProvideSort) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQueryAsCommand(fromjson("{find: 'testns', filter: {b : {$gte: 3}}, sort: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$gte: 3}}, node: {ixscan: {pattern: {a: 1, b: 1}, bounds: {a: "
        "[['MinKey','MaxKey',true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest,
       MultikeyIndexScanWithBoundsOnIndexWithoutSharedPrefixCanProvideSortDifferentIndex) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQueryAsCommand(fromjson("{find: 'testns', filter: {a : {$eq: 3}}, sort: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, bounds: {a: "
        "[[3,3,true,true]], b: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest,
       MultikeyIndexScanWithFilterAndSortOnFieldsThatSharePrefixCannotProvideSort) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U}, {0U}};
    addIndex(BSON("a" << 1 << "a.b" << 1), multikeyPaths);
    runQueryAsCommand(fromjson("{find: 'testns', filter: {a : {$gte: 3}}, sort: {'a.b': 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {'a.b': 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, 'a.b': 1}, filter: null, bounds: "
        "{a: [[3, Infinity, true,true]], 'a.b': [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest,
       MultikeyIndexScanWithFilterAndSortOnFieldsThatSharePrefixCannotProvideSortDifferentIndex) {
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    MultikeyPaths multikeyPaths{{0U, 1U}, {0U}};
    addIndex(BSON("a.b" << 1 << "a" << 1), multikeyPaths);
    runQueryAsCommand(fromjson("{find: 'testns', filter: {'a.b' : {$gte: 3}}, sort: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: {ixscan: {pattern: {'a.b': 1, a: 1}, filter: null, bounds: "
        "{'a.b': [[3, Infinity, true,true]], a: [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchValueNENull) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$elemMatch: {$ne: null}}}"));

    // We can't use the index because we would exclude {a: []} which should match.
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$ne: null}}}, node: {"
        "  ixscan: {pattern: {a:1}, bounds: {"
        "    a: [['MinKey',null,true,false], [null,'MaxKey',false,true]]"
        "}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchValueNotGteOrNotLteNull) {
    addIndex(BSON("a" << 1));
    runQuery(fromjson("{a: {$elemMatch: {$not: {$gte: null}}}}"));

    const auto collScanSol = "{cscan: {dir: 1}}";
    const auto ixScanSol =
        "{fetch: {node: {"
        "  ixscan: {pattern: {a:1}, bounds: {"
        "    a: [['MinKey',null,true,false], [null,'MaxKey',false,true]]"
        "}}}}}";

    assertNumSolutions(2U);
    assertSolutionExists(collScanSol);
    assertSolutionExists(ixScanSol);

    runQuery(fromjson("{a: {$elemMatch: {$not: {$lte: null}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(collScanSol);
    assertSolutionExists(ixScanSol);
}

TEST_F(QueryPlannerTest, NENullOnMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$ne: null}}"));

    // We can't use the index because we would exclude {a: []} which should match.
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ElemMatchValueNENullOnMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {$ne: null}}}"));

    // We should be able to use the index because of the value $elemMatch.
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$ne: null}}}, node: {"
        "  ixscan: {pattern: {a: 1}, bounds: {"
        "    a: [['MinKey',null,true,false], [null,'MaxKey',false,true]]"
        "}}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchObjectNENullOnMultikeyIndex) {
    // true means multikey
    addIndex(BSON("a.b" << 1), true);
    runQuery(fromjson("{a: {$elemMatch: {b: {$ne: null}}}}"));

    // We can't use the index because we would exclude {a: [{b: []}]} which should match.
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ElemMatchObjectNENullWithSuffixOfElemMatchMultiKey) {
    MultikeyPaths multikeyPaths{{0U, 1U}};
    addIndex(BSON("a.b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {b: {$ne: null}}}}"));

    // We can't use the index because we would exclude {a: [{b: []}]} which should match.
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNullWithProjectionMultiKeyOnOtherPath) {
    MultikeyPaths multikeyPaths{{0U}, {}};
    addIndex(BSON("a" << 1 << "c.d" << 1), multikeyPaths);
    runQuerySortProj(fromjson("{'a': {$gt: 'foo'}, 'c.d': {$ne: null}}"),
                     BSONObj(),
                     fromjson("{_id: 0, 'c.d': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, 'c.d': 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'c.d': 1}, node: {"
        "  ixscan: {filter: null, pattern: {'a': 1, 'c.d': 1}, bounds: {"
        "    'a': [['foo',{},false,false]], "
        "    'c.d':[['MinKey',null,true,false],[null,'MaxKey',false,true]]"
        "}}}}}");
}

// Test for older versions of indexes where it is possible to have empty MultikeyPaths,
// but still the index is considered multikey. In that case the index should not be
// considered to evaluate an $elemMatch predicate with a positional path component.
TEST_F(QueryPlannerTest, ElemMatchValueMultikeyIndexEmptyMultikeyPaths) {
    addIndex(BSON("a.0" << 1), true);
    runQuery(fromjson("{'a.0': {$elemMatch: {$eq: 42}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ElemMatchValuePositionalIndexPath1) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}};
    addIndex(BSON("f1.0" << 1), multikeyPaths);
    runQuery(fromjson("{'f1.0': {$elemMatch: {$eq: 42}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ElemMatchValuePositionalIndexPath2) {
    MultikeyPaths multikeyPaths{{0U}};
    addIndex(BSON("f1.0" << 1), multikeyPaths);
    runQuery(fromjson("{'f1.0': {$elemMatch: {$eq: 42}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, ElemMatchValuePositionalIndexPath3) {
    MultikeyPaths multikeyPaths{MultikeyComponents{}};
    addIndex(BSON("0" << 1), multikeyPaths);
    runQuery(fromjson("{'0': {$elemMatch: {$eq: 42}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'0': 1}, bounds: "
        "{'0': [[42,42,true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNullMultiKey) {
    const bool isMultiKey = true;
    addIndex(BSON("a.b" << 1 << "c.d" << 1), isMultiKey);
    runQuery(fromjson("{'a.b': {$gt: 'foo'}, 'c.d': {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {"
        "  filter: {'c.d': {$ne: null}},"
        "  node: {ixscan: {"
        "    filter: null,"
        "    pattern: {'a.b': 1, 'c.d': 1},"
        "    bounds: {"
        "      'a.b': [['foo',{},false,false]], "
        "      'c.d':[['MinKey','MaxKey',true,true]]"
        "    }"
        "  }}"
        "}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNullMultiKeyPaths) {
    const MultikeyPaths multikeyPaths = {{}, {0}};  // 'c' is multikey.
    addIndex(BSON("a.b" << 1 << "c.d" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': {$gt: 'foo'}, 'c.d': {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {"
        "  filter: {'c.d': {$ne: null}},"
        "  node: {ixscan: {"
        "    filter: null,"
        "    pattern: {'a.b': 1, 'c.d': 1},"
        "    bounds: {"
        "      'a.b': [['foo',{},false,false]], "
        "      'c.d':[['MinKey','MaxKey',true,true]]"
        "    }"
        "  }}"
        "}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsDottedNotEqualsNullMultiKeyWithProjection) {
    const bool isMultiKey = true;
    addIndex(BSON("a.b" << 1 << "c.d" << 1), isMultiKey);
    runQuerySortProj(fromjson("{'a.b': {$gt: 'foo'}, 'c.d': {$ne: null}}"),
                     BSONObj(),
                     fromjson("{_id: 0, 'c.d': 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, 'c.d': 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'c.d': 1}, node: {"
        "fetch: {filter: {'c.d': {$ne: null}}, node: {ixscan: {filter: null, pattern: "
        "{'a.b': 1, 'c.d': 1}, bounds: {'a.b': [['foo',{},false,false]], "
        "'c.d':[['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexBoundsNotEqualsNullReverseIndex) {
    addIndex(BSON("a" << 1 << "b" << -1 << "c" << 1));
    runQuery(fromjson("{a: {$gt: 'foo'}, b: {$ne: null}, c: {$ne: null}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {"
        "  filter: null,"
        "  node: {"
        "    ixscan: {"
        "      filter: null, "
        "      pattern: {a: 1, b: -1, c: 1},"
        "      bounds: {"
        "        a: [['foo', {}, false, false]],"
        "        b: [['MaxKey', null, true, false], [null, 'MinKey', false, true]],"
        "        c: [['MinKey', null, true, false], [null, 'MaxKey', false, true]]"
        "      },"
        "      dir: 1"
        "    }"
        "  }"
        "}}");
}

TEST_F(QueryPlannerTest, MustFetchBeforeDottedMultiKeyPathSort) {
    addIndex(BSON("a.x" << 1), /* multikey= */ true);
    runQuerySortProj(fromjson("{'a.x': 4}"), BSON("a.x" << 1), BSONObj());

    assertNumSolutions(3U);
    assertSolutionExists(
        "{sort: {pattern: {'a.x': 1}, limit: 0, type: 'simple', node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {"
        "  pattern: {'a.x': 1},"
        "  limit: 0,"
        "  type: 'simple',"
        "  node: {"
        "   fetch: {"
        "     filter: {'a.x': 4},"
        "     node: {"
        "       ixscan: {"
        "         filter: null, "
        "         pattern: {'a.x': 1},"
        "         bounds: {"
        "           'a.x': [['MinKey', 'MaxKey', true, true]]"
        "         },"
        "         dir: 1"
        "       }"
        "     }"
        "   }"
        "  }"
        "}}");
    assertSolutionExists(
        "{sort: {"
        "  pattern: {'a.x': 1},"
        "  limit: 0,"
        "  type: 'simple',"
        "  node: {"
        "   fetch: {"
        "     filter: null,"
        "     node: {"
        "       ixscan: {"
        "         filter: null, "
        "         pattern: {'a.x': 1},"
        "         bounds: {"
        "           'a.x': [[4, 4, true, true]]"
        "         },"
        "         dir: 1"
        "       }"
        "     }"
        "   }"
        "  }"
        "}}");

    // Now without a query predicate.
    runQuerySortProj(BSONObj(), BSON("a.x" << 1), BSONObj());

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {'a.x': 1}, limit: 0, type: 'simple', node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{sort: {"
        "  pattern: {'a.x': 1},"
        "  limit: 0,"
        "  type: 'simple',"
        "  node: {"
        "   fetch: {"
        "     filter: null,"
        "     node: {"
        "       ixscan: {"
        "         filter: null, "
        "         pattern: {'a.x': 1},"
        "         bounds: {"
        "           'a.x': [['MinKey', 'MaxKey', true, true]]"
        "         },"
        "         dir: 1"
        "       }"
        "     }"
        "   }"
        "  }"
        "}}");
}
}  // namespace
