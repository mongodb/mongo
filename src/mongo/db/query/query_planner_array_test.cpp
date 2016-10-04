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

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"

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

    ASSERT_EQUALS(getNumSolutions(), 3U);
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
    runQuery(fromjson("{a: {$elemMatch: {b: /foo/}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:/foo/}}}, node: "
        "{ixscan: {filter: null, pattern: {'a.b': 1}}}}}");
}

// SERVER-14180
TEST_F(QueryPlannerTest, ElemMatchEmbeddedRegexAnd) {
    addIndex(BSON("a.b" << 1));
    runQuery(fromjson("{a: {$elemMatch: {b: /foo/}}, z: 1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:/foo/}}, z:1}, node: "
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

// SERVER-14625: Make sure we construct bounds properly for $elemMatch object with a
// negation inside.
TEST_F(QueryPlannerTest, ElemMatchWithNotInside2) {
    addIndex(BSON("a.b" << 1 << "a.c" << 1));
    runQuery(fromjson("{d: 1, a: {$elemMatch: {c: {$ne: 3}, b: 4}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {d: 1, a: {$elemMatch: {c: {$ne: 3}, b: 4}}}, node:"
        "{ixscan: {filter: null, pattern: {'a.b': 1, 'a.c': 1}, bounds:"
        "{'a.b': [[4,4,true,true]],"
        " 'a.c': [['MinKey',3,true,false],"
        "[3,'MaxKey',false,true]]}}}}}");
}

// SERVER-13789
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedOr) {
    addIndex(BSON("bar.baz" << 1));
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {$or: [{baz: 2}]}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{foo:1},"
        "{bar:{$elemMatch:{$or:[{baz:2}]}}}]}, "
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
        "{bar: {$elemMatch: {$or: [{$and: [{baz:2}, {z:3}]}]}}}]},"
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

// SERVER-13789
TEST_F(QueryPlannerTest, ElemMatchIndexedNestedNE) {
    addIndex(BSON("bar.baz" << 1));
    runQuery(fromjson("{foo: 1, $and: [{bar: {$elemMatch: {baz: {$ne: 2}}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {$and: [{foo:1},"
        "{bar:{$elemMatch:{baz:{$ne:2}}}}]}, "
        "node: {ixscan: {pattern: {'bar.baz': 1}, "
        "bounds: {'bar.baz': [['MinKey',2,true,false], "
        "[2,'MaxKey',false,true]]}}}}}");
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
    runQuery(fromjson("{'a.b': {$elemMatch: {$gte: 1, $lte: 1}}}}}"));

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

// SERVER-13422: check that we plan $elemMatch object correctly with
// index intersection.
TEST_F(QueryPlannerTest, ElemMatchIndexIntersection) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
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
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    // true means multikey
    addIndex(BSON("a" << 1), true);

    runQuery(fromjson("{a: {$elemMatch: {$not: {$mod: [2, 0]}}}}"));

    // There are no indexed solutions, because negations of $mod are not indexable.
    assertNumSolutions(0);
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
    MultikeyPaths multikeyPaths{std::set<size_t>{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, 10, true, false]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnFirstFieldWhenItAndSharedPrefixAreNotMultikey) {
    MultikeyPaths multikeyPaths{std::set<size_t>{}, {1U}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[0, 10, true, false]], 'a.c': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsWhenFirstFieldIsMultikey) {
    MultikeyPaths multikeyPaths{{0U}, std::set<size_t>{}};
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
    MultikeyPaths multikeyPaths{{0U}, std::set<size_t>{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$elemMatch: {$gte: 0, $lt: 10}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: {$elemMatch: {$gte: 0, $lt: 10}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[0, 10, true, false]], b: [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanComplementBoundsOnFirstFieldWhenItIsMultikeyAndHasNotEqualExpr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{{0U}, std::set<size_t>{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: {$ne: 3}, b: 2}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [['MinKey', 3, true, false], [3, 'MaxKey', false, true]], "
        "b: [[2, 2, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenFirstFieldIsMultikeyAndHasNotInsideElemMatch) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{{0U}, std::set<size_t>{}};
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
    MultikeyPaths multikeyPaths{{0U}, std::set<size_t>{}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsOnSecondFieldWhenItAndSharedPrefixAreNotMultikey) {
    MultikeyPaths multikeyPaths{{1U}, std::set<size_t>{}};
    addIndex(BSON("a.b" << 1 << "a.c" << 1), multikeyPaths);
    runQuery(fromjson("{'a.b': 2, 'a.c': {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {'a.b': 2, 'a.c': {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'a.b': 1, 'a.c': 1}, "
        "bounds: {'a.b': [[2, 2, true, true]], 'a.c': [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CannotIntersectBoundsWhenSecondFieldIsMultikey) {
    MultikeyPaths multikeyPaths{std::set<size_t>{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$gte: 0, $lt: 10}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$gte: 0, $lt: 10}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[-Infinity, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenSecondFieldIsMultikeyButHasElemMatch) {
    MultikeyPaths multikeyPaths{std::set<size_t>{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$elemMatch: {$gte: 0, $lt: 10}}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1, filter: {a: 2, b: {$elemMatch: {$gte: 0, $lt: 10}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[0, 10, true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, CanComplementBoundsOnSecondFieldWhenItIsMultikeyAndHasNotEqualExpr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{std::set<size_t>{}, {0U}};
    addIndex(BSON("a" << 1 << "b" << 1), multikeyPaths);
    runQuery(fromjson("{a: 2, b: {$ne: 3}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], "
        "b: [['MinKey', 3, true, false], [3, 'MaxKey', false, true]]}}}}}");
}

TEST_F(QueryPlannerTest, CanIntersectBoundsWhenSecondFieldIsMultikeyAndHasNotInsideElemMatch) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    MultikeyPaths multikeyPaths{std::set<size_t>{}, {0U}};
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

}  // namespace
