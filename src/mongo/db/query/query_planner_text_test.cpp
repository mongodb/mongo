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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

//
// Text
// Creating an FTS index {a:1, b:"text", c:1} actually
// creates an index with spec {a:1, _fts: "text", _ftsx: 1, c:1}.
// So, the latter is what we pass in to the planner.
//
// PS. You can also do {a:1, b:"text", d:"text", c:1} and it will create an index with the same
// key pattern.
//

// Basic test that it works.
TEST_F(QueryPlannerTest, SimpleText) {
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(fromjson("{$text: {$search: 'blah'}}"));

    assertNumSolutions(1);
    assertSolutionExists("{text: {search: 'blah'}}");
}

// If you create an index {a:1, b: "text"} you can't use it for queries on just 'a'.
TEST_F(QueryPlannerTest, CantUseTextUnlessHaveTextPred) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    // No table scans allowed so there is no solution.
    runInvalidQuery(fromjson("{a:1}"));
    assertNoSolutions();
}

// But if you create an index {a:1, b:"text"} you can use it if it has a pred on 'a'
// and a text query.
TEST_F(QueryPlannerTest, HaveOKPrefixOnTextIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));

    runQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));
    assertNumSolutions(1);
    assertSolutionExists("{text: {prefix: {a:1}, search: 'blah'}}");

    // TODO: Do we want to $or a collection scan with a text search?
    // runQuery(fromjson("{$or: [{b:1}, {a:1, $text: {$search: 'blah'}}]}"));
    // assertNumSolutions(1);

    runQuery(fromjson("{$or: [{_id:1}, {a:1, $text: {$search: 'blah'}}]}"));
    assertNumSolutions(1);
}

// But the prefixes must be points.
TEST_F(QueryPlannerTest, HaveBadPrefixOnTextIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    runInvalidQuery(fromjson("{a:{$gt: 1}, $text:{$search: 'blah'}}"));

    runInvalidQuery(fromjson("{$text: {$search: 'blah'}}"));

    runInvalidQuery(fromjson("{$or: [{a:1}, {$text: {$search: 'blah'}}]}"));
}

// Outside predicates are not yet pushed into contained ORs for text indexes.
TEST_F(QueryPlannerTest, PrefixOnTextIndexIsOutsidePred) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    addIndex(BSON("b" << 1));
    runInvalidQuery(fromjson("{$and: [{a: 5}, {$or: [{$text: {$search: 'blah'}}, {b: 6}]}]}"));
}

// There can be more than one prefix, but they all require points.
TEST_F(QueryPlannerTest, ManyPrefixTextIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "b" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));

    // Both points.
    runQuery(fromjson("{a:1, b:1, $text:{$search: 'blah'}}"));
    assertSolutionExists("{text: {prefix: {a:1, b:1}, search: 'blah'}}");
    assertNumSolutions(1);

    // Missing a.
    runInvalidQuery(fromjson("{b:1, $text:{$search: 'blah'}}"));

    // Missing b.
    runInvalidQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));

    // a is not a point
    runInvalidQuery(fromjson("{a:{$gt: 1}, b:1, $text:{$search: 'blah'}}"));

    // b is not a point
    runInvalidQuery(fromjson("{a:1, b:{$gt: 1}, $text:{$search: 'blah'}}"));
}

// And, suffixes.  They're optional and don't need to be points.
TEST_F(QueryPlannerTest, SuffixOptional) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1 << "b" << 1));

    runQuery(fromjson("{a:1, $text:{$search: 'blah'}}"));
    assertNumSolutions(1);
    assertSolutionExists("{text: {prefix: {a:1}, search: 'blah'}}");

    runQuery(fromjson("{a:1, b:{$gt: 7}, $text:{$search: 'blah'}}"));
    assertSolutionExists("{text: {prefix: {a:1}, filter: {b: {$gt: 7}}, search: 'blah'}}");
    assertNumSolutions(1);
}

TEST_F(QueryPlannerTest, RemoveFromSubtree) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1 << "b" << 1));

    runQuery(fromjson("{a:2, $or: [{a:1}, {b:7}], $text:{$search: 'blah'}}"));
    assertNumSolutions(1);

    assertSolutionExists(
        "{fetch: {filter: {$or:[{a:1},{b:7}]},"
        "node: {text: {prefix: {a:2}, search: 'blah'}}}}");
}

// Text is quite often multikey.  None of the prefixes can be arrays, and suffixes are indexed
// as-is, so we should compound even if it's multikey.
TEST_F(QueryPlannerTest, CompoundPrefixEvenIfMultikey) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "b" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1),
             true);

    // Both points.
    runQuery(fromjson("{a:1, b:1, $text:{$search: 'blah'}}"));
    assertNumSolutions(1);
    assertSolutionExists("{text: {prefix: {a:1, b:1}, search: 'blah'}}");
}

TEST_F(QueryPlannerTest, IndexOnOwnFieldButNotLeafPrefix) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1 << "b" << 1));

    // 'a' is not an EQ so it doesn't compound w/the text pred.  We also shouldn't use the text
    // index to satisfy it w/o the text query.
    runInvalidQuery(fromjson("{a:{$elemMatch:{$gt: 0, $lt: 2}}, $text:{$search: 'blah'}}"));
}

TEST_F(QueryPlannerTest, IndexOnOwnFieldButNotLeafSuffixNoPrefix) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1 << "b" << 1));

    runQuery(fromjson("{b:{$elemMatch:{$gt: 0, $lt: 2}}, $text:{$search: 'blah'}}"));
    assertNumSolutions(1);
}

TEST_F(QueryPlannerTest, TextInsideAndWithCompoundIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    runQuery(fromjson("{$and: [{a: 3}, {$text: {$search: 'foo'}}], a: 3}"));

    assertNumSolutions(1U);
    assertSolutionExists("{text: {prefix: {a:3}, search: 'foo'}}");
}

// SERVER-15639: Test that predicates on index prefix fields which are not assigned to the index
// prefix are correctly included in the solution node filter.
TEST_F(QueryPlannerTest, TextInsideAndWithCompoundIndexAndMultiplePredsOnIndexPrefix) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    runQuery(fromjson("{$and: [{a: 1}, {a: 2}, {$text: {$search: 'foo'}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists("{text: {prefix: {a: 1}, search: 'foo', filter: {a: 2}}}");
}

// SERVER-13039: Test that we don't generate invalid solutions when the TEXT node
// is buried beneath a logical node.
TEST_F(QueryPlannerTest, TextInsideOrBasic) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(fromjson("{a: 0, $or: [{_id: 1}, {$text: {$search: 'foo'}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:0}, node: {or: {nodes: ["
        "{text: {search: 'foo'}}, "
        "{ixscan: {filter: null, pattern: {_id: 1}}}]}}}}");
}

// SERVER-13039
TEST_F(QueryPlannerTest, TextInsideOrWithAnotherOr) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{$and: [{$or: [{a: 3}, {a: 4}]}, "
                 "{$or: [{$text: {$search: 'foo'}}, {a: 5}]}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$in: [3, 4]}}, node: "
        "{or: {nodes: ["
        "{text: {search: 'foo'}}, "
        "{ixscan: {filter: null, pattern: {a: 1}}}]}}}}");
}

// SERVER-13039
TEST_F(QueryPlannerTest, TextInsideOrOfAnd) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{$or: [{a: {$gt: 1, $gt: 2}}, "
                 "{a: {$gt: 3}, $text: {$search: 'foo'}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}, bounds: "
        "{a: [[2,Infinity,false,true]]}}}, "
        "{fetch: {filter: {a:{$gt:3}}, node: "
        "{text: {search: 'foo'}}}}]}}}}");
}

// SERVER-13039
TEST_F(QueryPlannerTest, TextInsideAndOrAnd) {
    RAIIServerParameterControllerForTest controller(
        "internalQueryEnableBooleanExpressionsSimplifier", true);

    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{a: 1, $or: [{a:2}, {b:2}, "
                 "{a: 1, $text: {$search: 'foo'}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        R"--({fetch: {filter: {a: 1}, node: {or: {nodes: [
                {text: {search: 'foo'}},
                {ixscan: {pattern: {a: 1}}},
                {ixscan: {pattern: {b: 1}}}
            ]}}}})--");
}

// SERVER-13039
TEST_F(QueryPlannerTest, TextInsideAndOrAnd_DisabledSimplifier) {
    RAIIServerParameterControllerForTest controller(
        "internalQueryEnableBooleanExpressionsSimplifier", false);

    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{a: 1, $or: [{a:2}, {b:2}, "
                 "{a: 1, $text: {$search: 'foo'}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:1}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a:1}}}, "
        "{fetch: {filter: {a:1}, node: {text: {search: 'foo'}}}}, "
        "{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
}

// SERVER-13039
TEST_F(QueryPlannerTest, TextInsideAndOrAndOr) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{$or: [{a: {$gt: 1, $gt: 2}}, "
                 "{a: {$gt: 3}, $or: [{$text: {$search: 'foo'}}, "
                 "{a: 6}]}], "
                 "a: 5}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:5}, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {a: 1}}}, "
        "{fetch: {filter: {a:{$gt:3}}, node: {or: {nodes: ["
        "{text: {search: 'foo'}}, "
        "{ixscan: {filter: null, pattern: {a: 1}}}]}}}}]}}}}");
}

// If only one branch of the $or can be indexed, then no indexed
// solutions are generated, even if one branch is $text.
TEST_F(QueryPlannerTest, TextInsideOrOneBranchNotIndexed) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runInvalidQuery(fromjson("{a: 1, $or: [{b: 2}, {$text: {$search: 'foo'}}]}"));

    assertNoSolutions();
}

// If the unindexable $or is not the one containing the $text predicate,
// then we should still be able to generate an indexed solution.
TEST_F(QueryPlannerTest, TextInsideOrWithAnotherUnindexableOr) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1));
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{$and: [{$or: [{a: 1}, {b: 1}]}, "
                 "{$or: [{a: 2}, {$text: {$search: 'foo'}}]}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {$or:[{a:1},{b:1}]}, node: {or: {nodes: ["
        "{text: {search: 'foo'}}, "
        "{ixscan: {filter: null, pattern: {a:1}}}]}}}}");
}

TEST_F(QueryPlannerTest, AndTextWithGeoNonNear) {
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(
        fromjson("{$text: {$search: 'foo'}, a: {$geoIntersects: {$geometry: "
                 "{type: 'Point', coordinates: [3.0, 1.0]}}}}"));

    // Mandatory text index is used, and geo predicate becomes a filter.
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {text: {search: 'foo'}}}}");
}

// SERVER-13960: $text beneath $or with exact predicates.
TEST_F(QueryPlannerTest, OrTextExact) {
    addIndex(BSON("pre" << 1 << "_fts"
                        << "text"
                        << "_ftsx" << 1));
    addIndex(BSON("other" << 1));
    runQuery(fromjson("{$or: [{$text: {$search: 'dave'}, pre: 3}, {other: 2}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{text: {search: 'dave', prefix: {pre: 3}}},"
        "{ixscan: {filter: null, pattern: {other: 1}}}]}}}}");
}

// SERVER-13960: $text beneath $or with an inexact covered predicate.
TEST_F(QueryPlannerTest, OrTextInexactCovered) {
    addIndex(BSON("pre" << 1 << "_fts"
                        << "text"
                        << "_ftsx" << 1));
    addIndex(BSON("other" << 1));
    runQuery(fromjson("{$or: [{$text: {$search: 'dave'}, pre: 3}, {other: /bar/}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{text: {search: 'dave', prefix: {pre: 3}}},"
        "{ixscan: {filter: {$or: [{other: /bar/}]}, "
        "pattern: {other: 1}}}]}}}}");
}

TEST_F(QueryPlannerTest, TextCaseSensitive) {
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(fromjson("{$text: {$search: 'blah', $caseSensitive: true}}"));

    assertNumSolutions(1);
    assertSolutionExists("{text: {search: 'blah', caseSensitive: true}}");
}

TEST_F(QueryPlannerTest, TextDiacriticSensitive) {
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));
    runQuery(fromjson("{$text: {$search: 'blah', $diacriticSensitive: true}}"));

    assertNumSolutions(1);
    assertSolutionExists("{text: {search: 'blah', diacriticSensitive: true}}");
}

TEST_F(QueryPlannerTest, SortKeyMetaProjectionWithTextScoreMetaSort) {
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1));

    runQuerySortProj(fromjson("{$text: {$search: 'foo'}}"),
                     fromjson("{a: {$meta: 'textScore'}}"),
                     fromjson("{a: {$meta: 'textScore'}, b: {$meta: 'sortKey'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {a: {$meta: 'textScore'}, b: {$meta: 'sortKey'}}, node: "
        "{sort: {limit: 0, pattern: {a: {$meta: 'textScore'}}, type: 'default', node: "
        "{text: {search: 'foo'}}}}}}");
}

TEST_F(QueryPlannerTest, PredicatesOverLeadingFieldsWithSharedPathPrefixHandledCorrectly) {
    const bool multikey = true;
    addIndex(BSON("a.x" << 1 << "a.y" << 1 << "b.x" << 1 << "b.y" << 1 << "_fts"
                        << "text"
                        << "_ftsx" << 1),
             multikey);

    runQuery(fromjson("{'a.x': 1, 'a.y': 2, 'b.x': 3, 'b.y': 4, $text: {$search: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{text: {search: 'foo', prefix: {'a.x': 1, 'a.y': 2, 'b.x': 3, 'b.y': 4}}}");
}

TEST_F(QueryPlannerTest, EqualityToArrayOverLeadingFieldHandledCorrectly) {
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));

    runQuery(fromjson("{a: [1, 2, 3], $text: {$search: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{text: {search: 'foo', prefix: {a: [1, 2, 3]}}}");
}

TEST_F(QueryPlannerTest, EqualityToArrayOverLeadingFieldHandledCorrectlyWithMultikeyTrue) {
    const bool multikey = true;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1),
             multikey);

    runQuery(fromjson("{a: [1, 2, 3], $text: {$search: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{text: {search: 'foo', prefix: {a: [1, 2, 3]}}}");
}

TEST_F(QueryPlannerTest, InexactFetchPredicateOverTrailingFieldHandledCorrectly) {
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1 << "b" << 1));

    runQuery(fromjson("{a: 3, $text: {$search: 'foo'}, b: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: true}}, node: {text: {search: 'foo', prefix: {a: 3}}}}}");
}

TEST_F(QueryPlannerTest, InexactFetchPredicateOverTrailingFieldHandledCorrectlyMultikeyTrue) {
    const bool multikey = true;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1 << "b" << 1),
             multikey);

    runQuery(fromjson("{a: 3, $text: {$search: 'foo'}, b: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: true}}, node: {text: {search: 'foo', prefix: {a: 3}}}}}");
}

TEST_F(QueryPlannerTest, ExprEqCannotUsePrefixOfTextIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));

    runInvalidQuery(fromjson("{a: {$_internalExprEq: 3}, $text: {$search: 'blah'}}"));
}

TEST_F(QueryPlannerTest, ExprEqCanUseSuffixOfTextIndex) {
    params.mainCollectionInfo.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("_fts" << "text"
                         << "_ftsx" << 1 << "a" << 1));

    runQuery(fromjson("{a: {$_internalExprEq: 3}, $text: {$search: 'blah'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{text: {search: 'blah', prefix: {}, filter: {a: {$_internalExprEq: 3}}}}");
}

TEST_F(QueryPlannerTest, CantExplodeMetaSort) {
    addIndex(BSON("a" << 1 << "b" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));
    runInvalidQuerySortProj(
        fromjson("{$text: {$search: 'keyword'}, a: {$in: [1, 2]}, b: {$in: [3, 4]}}"),
        fromjson("{c: {$meta: 'textScore'}}"),
        fromjson("{c: {$meta: 'textScore'}}"));
}

}  // namespace
