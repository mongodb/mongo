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

#include "mongo/platform/basic.h"

#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {

namespace wcp = ::mongo::wildcard_planning;

const std::string kIndexName = "indexName";

/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner with
 * a view of the available $** indexes.
 */
class QueryPlannerWildcardTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // We're interested in testing plans that use a $** index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void addWildcardIndex(BSONObj keyPattern,
                          const OrderedPathSet& multikeyPathSet = {},
                          BSONObj wildcardProjection = BSONObj{},
                          MatchExpression* partialFilterExpr = nullptr,
                          CollatorInterface* collator = nullptr,
                          const std::string& indexName = kIndexName) {
        // Convert the set of std::string to a set of FieldRef.
        std::set<FieldRef> multikeyFieldRefs;
        for (auto&& path : multikeyPathSet) {
            ASSERT_TRUE(multikeyFieldRefs.emplace(path).second);
        }
        ASSERT_EQ(multikeyPathSet.size(), multikeyFieldRefs.size());

        const bool isMultikey = !multikeyPathSet.empty();
        BSONObj infoObj = BSON("wildcardProjection" << wildcardProjection);

        _proj = WildcardKeyGenerator::createProjectionExecutor(keyPattern, wildcardProjection);

        params.indices.push_back({std::move(keyPattern),
                                  IndexType::INDEX_WILDCARD,
                                  IndexDescriptor::kLatestIndexVersion,
                                  isMultikey,
                                  {},  // multikeyPaths
                                  std::move(multikeyFieldRefs),
                                  false,  // sparse
                                  false,  // unique
                                  IndexEntry::Identifier{indexName},
                                  partialFilterExpr,
                                  std::move(infoObj),
                                  collator,
                                  _proj.get_ptr()});
    }

    boost::optional<WildcardProjection> _proj;
};

//
// General planning tests.
//

DEATH_TEST_F(QueryPlannerWildcardTest, CannotExpandPreExpandedWildcardIndexEntry, "Invariant") {
    addWildcardIndex(BSON("$**" << 1));
    ASSERT_EQ(params.indices.size(), 2U);

    // Expand the $** index and add the expanded entry to the list of available indices.
    std::vector<IndexEntry> expandedIndex;
    wcp::expandWildcardIndexEntry(params.indices.back(), {"a"}, &expandedIndex);
    ASSERT_EQ(expandedIndex.size(), 1U);
    params.indices.push_back(expandedIndex.front());

    // Now run a query. This will invariant when the planner expands the expanded index.
    runQuery(fromjson("{a: 1}"));
}

//
// Null comparison and existence tests.
//

TEST_F(QueryPlannerWildcardTest, ExistsTrueQueriesUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ExistsFalseQueriesDontUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, EqualsNullQueriesDontUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullQueriesUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$ne: null}}, node: {ixscan: {pattern: {$_path: 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true], ['x.','x/',true,false]],"
        "x: [['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullQueriesDoNotUseWildcardIndexesWhenMultiKey) {
    addWildcardIndex(BSON("$**" << 1), {"x"});

    runQuery(fromjson("{'x': {$ne: null}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullInElemMatchQueriesUseWildcardIndexesWhenMultiKey) {
    addWildcardIndex(BSON("$**" << 1), {"x"});

    runQuery(fromjson("{'x': {$elemMatch: {$ne: null}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {$_path: 1, 'x': 1},"
        "bounds: {'$_path': [['x','x',true,true], ['x.','x/',true,false]],"
        "'x': [['MinKey', 'MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullInElemMatchObjectSparseMultiKeyAboveElemMatch) {
    addWildcardIndex(BSON("$**" << 1), {"a", "a.b"});

    runQuery(fromjson("{'a.b': {$elemMatch: {'c.d': {$ne: null}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, 'a.b.c.d': 1},"
        "bounds: {'$_path': [['a.b.c.d','a.b.c.d',true,true], ['a.b.c.d.','a.b.c.d/',true,false]],"
        "'a.b.c.d': [['MinKey', 'MaxKey', true, true]]"
        "}}}}}");
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullInElemMatchObjectSparseMultiKeyBelowElemMatch) {
    // "a.b.c" being multikey will prevent us from using the index since $elemMatch doesn't do
    // implicit array traversal.
    addWildcardIndex(BSON("$**" << 1), {"a.b.c"});

    runQuery(fromjson("{'a.b': {$elemMatch: {'c.d': {$ne: null}}}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, NotEqualsNullAndExistsQueriesUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, EqualsNullAndExistsQueriesUseWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, EmptyBoundsWithWildcardIndexes) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lte: 5, $gte: 10}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

//
// Multikey planning tests.
//

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverMultikeyFieldNoElemMatch) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverMultikeyFieldWithElemMatch) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$gt: 0, $lt: 9}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$gt: 0, $lt: 9}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverNonMultikeyFieldWithMultikeyIndex) {
    addWildcardIndex(BSON("$**" << 1), {"b"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverNestedFieldWithFirstComponentMultikey) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{'a.b': {$gt: 0, $lt: 9}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverNestedFieldWithElemMatchObject) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       MultiplePredicatesOverNestedFieldWithElemMatchObjectBothComponentsMultikey) {
    addWildcardIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, MultiplePredicatesOverNestedFieldWithTwoElemMatches) {
    addWildcardIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ElemMatchOnInnermostMultikeyPathPermitsTightBounds) {
    addWildcardIndex(BSON("$**" << 1), {"a", "a.b", "a.b.c"});
    runQuery(fromjson("{'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b.c.d.e.f': 1},"
        "bounds: {'$_path': [['a.b.c.d.e.f','a.b.c.d.e.f',true,true]],"
        "'a.b.c.d.e.f': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, AllPredsEligibleForIndexUseGenerateCandidatePlans) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.b", "a.c"});
    runQuery(
        fromjson("{'a.b': {$gt: 0, $lt: 9}, 'a.c': {$gt: 11, $lt: 20}, d: {$gt: 31, $lt: 40}}"));

    assertNumSolutions(4U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0,$lt: 9},'a.c':{$gt:11},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.c': 1},"
        "bounds: {'$_path': [['a.c','a.c',true,true]], 'a.c': [[-Infinity,20,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0,$lt: 9},'a.c':{$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.c': 1},"
        "bounds: {'$_path': [['a.c','a.c',true,true]], 'a.c': [[11,Infinity,false,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0},'a.c':{$gt:11,$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$lt:9},'a.c':{$gt:11,$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, RangeIndexScan) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, RangeIndexScanEmptyRange) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 9, $lt: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], 'a': []}}}}}");
}

TEST_F(QueryPlannerWildcardTest, RangeIndexScanMinKeyMaxKey) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: {$minKey: 1}, $lt: {$maxKey: 1}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true], ['a.', 'a/', true, false]], 'a': [['MinKey', "
        "'MaxKey', false, false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, RangeIndexScanNestedField) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{'a.b': {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, EqualityIndexScan) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, EqualityIndexScanOverNestedField) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{'a.b': {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ExprEqCanUseIndex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$_internalExprEq: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ExprEqCanUseSparseIndexForEqualityToNull) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$_internalExprEq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$_internalExprEq: null}}, node: {ixscan: {pattern: {'$_path': 1, a: "
        "1}, bounds: {'$_path': [['a','a',true,true]], a: [[undefined,undefined,true,true], "
        "[null,null,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, PrefixRegex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: /^foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]],"
        "a: [['foo','fop',true,false], [/^foo/,/^foo/,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, NonPrefixRegex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: /foo/}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: {a: /foo/}, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]],"
        "a: [['',{},true,false], [/foo/,/foo/,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, GreaterThan) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gt: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, GreaterThanEqualTo) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gte: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, LessThan) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$lt: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,5,true,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, LessThanEqualTo) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$lte: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, Mod) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$mod: [2, 0]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: {a: {$mod: [2, 0]}}, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[NaN,Infinity, true, true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ExistsTrue) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true],['x.','x/',true,false]], x: "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ExistsFalseDoesNotUseIndex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, AndEqualityWithTwoPredicatesIndexesOnePath) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: 5, b: 10}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$eq: 10}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, OrEqualityWithTwoPredicatesUsesTwoPaths) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {b: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}, "
        "{ixscan: {filter: null, pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[10,10,true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerWildcardTest, OrWithOneRegularAndOneWildcardIndexPathUsesTwoIndexes) {
    addWildcardIndex(BSON("a.$**" << 1));
    addIndex(BSON("b" << 1));
    runQuery(fromjson("{$or: [{a: 5}, {b: 10}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}, "
        "{ixscan: {filter: null, pattern: {b: 1},"
        "bounds: {b: [[10,10,true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerWildcardTest, BasicSkip) {
    addWildcardIndex(BSON("$**" << 1));
    runQuerySkipLimit(BSON("a" << 5), 8, 0);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CoveredSkip) {
    addWildcardIndex(BSON("$**" << 1));
    runQuerySortProjSkipLimit(fromjson("{a: 5}"), BSONObj(), fromjson("{_id: 0, a: 1}"), 8, 0);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {skip: {n: 8, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, BasicLimit) {
    addWildcardIndex(BSON("$**" << 1));
    runQuerySkipLimit(BSON("a" << 5), 0, 5);

    assertNumSolutions(1U);
    assertSolutionExists(
        "{limit: {n: 5, node: {fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, BasicCovering) {
    addWildcardIndex(BSON("$**" << 1));
    runQuerySortProj(fromjson("{ x : {$gt: 1}}"), BSONObj(), fromjson("{_id: 0, x: 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, x: 1}, node: {ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true]], x: [[1,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, DottedFieldCovering) {
    addWildcardIndex(BSON("$**" << 1));
    runQuerySortProj(fromjson("{'a.b': 5}"), BSONObj(), fromjson("{_id: 0, 'a.b': 1}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, 'a.b': 1}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[5,5,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CoveredIxscanForCountOnIndexedPath) {
    params.options = QueryPlannerParams::IS_COUNT;
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: 5}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a': 1},"
        "bounds: {'$_path': [['a','a',true,true]], 'a': [[5,5,true,true]]}}}");
}

TEST_F(QueryPlannerWildcardTest, InBasic) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [1, 2]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true],[2,2,true,true]]}}}}}");
}

//
// Array tests.
//

TEST_F(QueryPlannerWildcardTest, EqualsEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: []}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: "
        "[[undefined,undefined,true,true],[[],[],true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, EqualsNonEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: [1]}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, EqualsNestedEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: [[]]}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, EqualsArrayWithValue) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: [[1]]}"));

    assertHasOnlyCollscan();
}

//
// $in tests.
//

TEST_F(QueryPlannerWildcardTest, InEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [[]]}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: "
        "[[undefined,undefined,true,true],[[],[],true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, InNonEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [[1]]}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, InNestedEmptyArray) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [[[]]]}}"));

    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, InArrayWithValue) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [[[1]]]}}"));

    assertHasOnlyCollscan();
}

// Logically equivalent to the preceding $in query.
// Indexed solution should be the same.
TEST_F(QueryPlannerWildcardTest, InBasicOrEquivalent) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{$or: [{a: 1}, {a: 2}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true],[2,2,true,true]]}}}}}");
}

//
// Partial index tests.
//

TEST_F(QueryPlannerWildcardTest, PartialIndexCanAnswerPredicateOnFilteredField) {
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    runQuery(fromjson("{a: {$gte: 5}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");

    runQuery(fromjson("{a: 5}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,5,true,true]]}}}}}");

    runQuery(fromjson("{a: {$gte: 1, $lte: 10}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, PartialIndexDoesNotAnswerPredicatesExcludedByFilter) {
    // Must keep 'filterObj' around since match expressions will store pointers into the BSON they
    // were parsed from.
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    runQuery(fromjson("{a: {$gte: -1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$lte: 10}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$eq: 0}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, PartialIndexCanAnswerPredicateOnUnrelatedField) {
    auto filterObj = fromjson("{a: {$gt: 0}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());

    // Test when the field query is not included by the partial filter expression.
    runQuery(fromjson("{b: {$gte: -1}, a: {$gte: 5}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gte: 5}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[-1,Infinity,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$gte: -1}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[5,Infinity,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, PartialIndexWithExistsTrueFilterCanAnswerExistenceQuery) {
    auto filterObj = fromjson("{x: {$exists: true}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, BSONObj{}, filterExpr.get());
    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, x: 1},"
        "bounds: {'$_path': [['x','x',true,true],['x.','x/',true,false]], x: "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

//
// Index intersection tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotParticipateInIndexIntersection) {
    // Enable both AND_SORTED and AND_HASH index intersection for this test.
    params.options |= QueryPlannerParams::INDEX_INTERSECTION;
    internalQueryPlannerEnableHashIntersection.store(true);

    // Add two standard single-field indexes.
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    // Run a point query on both fields and confirm that an AND_SORTED plan is generated.
    runQuery(fromjson("{a:10, b:10}"));
    // Three plans are generated: one IXSCAN for each index, and an AND_SORTED on both.
    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:10, b:10}, node: {andSorted: {nodes: [{ixscan: {filter: null, "
        "pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Run a range query on both fields and confirm that an AND_HASH plan is generated.
    runQuery(fromjson("{a:{$gt: 10}, b:{$gt: 10}}"));
    // Three plans are generated: one IXSCAN for each index, and an AND_HASH on both.
    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt: 10}}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt: 10}}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt: 10}, b:{$gt: 10}}, node: {andHash: {nodes: [{ixscan: "
        "{filter: null, pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");

    // Now add a $** index and re-run the tests.
    addWildcardIndex(BSON("$**" << 1));

    // First re-run the AND_SORTED test.
    runQuery(fromjson("{a:10, b:10}"));
    // Solution count has increased from 3 to 5, as $** 'duplicates' the {a:1} and {b:1} IXSCANS.
    assertNumSolutions(5U);
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    // The previous AND_SORTED solution is still present...
    assertSolutionExists(
        "{fetch: {filter: {a:10, b:10}, node: {andSorted: {nodes: [{ixscan: {filter: null, "
        "pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    // ... but there are no additional AND_SORTED solutions contributed by the $** index.
    assertSolutionExists(
        "{fetch: {filter: {a:10}, node: {ixscan: {filter: null, pattern: {$_path:1, b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:10}, node: {ixscan: {filter: null, pattern: {$_path:1, a:1}}}}}");

    // Now re-run the AND_HASH test.
    runQuery(fromjson("{a:{$gt: 10}, b:{$gt: 10}}"));
    // Solution count has increased from 3 to 5, as $** 'duplicates' the {a:1} and {b:1} IXSCANS.
    assertNumSolutions(5U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:10}}, node: {ixscan: {filter: null, pattern: {b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b:{$gt:10}}, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    // The previous AND_HASH solution is still present...
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:10}, b:{$gt:10}}, node: {andHash: {nodes: [{ixscan: "
        "{filter: null, pattern: {a:1}}},{ixscan: {filter: null, pattern: {b:1}}}]}}}}");
    // ... but there are no additional AND_HASH solutions contributed by the $** index.
    assertSolutionExists(
        "{fetch: {filter:{a:{$gt:10}}, node: {ixscan: {filter: null, pattern: {$_path:1, b:1}}}}}");
    assertSolutionExists(
        "{fetch: {filter:{b:{$gt:10}}, node: {ixscan: {filter: null, pattern: {$_path:1, a:1}}}}}");
}

//
// Wildcard and $text index tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupplyCandidatePlanForTextSearch) {
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("a" << 1 << "_fts"
                      << "text"
                      << "_ftsx" << 1));

    // Confirm that the wildcard index generates candidate plans for queries which do not include a
    // $text predicate.
    runQuery(fromjson("{a: 10, b: 10}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {b: 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, b: 1}}}}}");

    // Confirm that the wildcard index does not produce any candidate plans when a query includes a
    // $text predicate, even for non-$text predicates which may be present in the query.
    runQuery(fromjson("{a: 10, b: 10, $text: {$search: 'banana'}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: 10}, node: {text: {prefix: {a: 10}, search: 'banana'}}}}");
}

//
// Negation tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardDoesNotSupportNegationPredicate) {
    // Wildcard indexes can't support negation queries because they are sparse, and {a: {$ne: 5}}
    // will match documents which don't have an "a" field.
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$ne: 5}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$not: {$gt: 3, $lt: 5}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest,
       WildcardDoesNotSupportNegationPredicateInsideElemMatchMultiKeyPath) {
    // Logically, there's no reason a (sparse) wildcard index could not support a negation inside a
    // "$elemMatch value", but it is not something we've implemented.
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$ne: 5}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$elemMatch: {$not: {$gt: 3, $lt: 5}}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, WildcardDoesNotSupportNegationPredicateInsideElemMatch) {
    // Test the case where we use $elemMatch on a path which isn't even multikey. In this case,
    // we'd know up front that the results would be empty, but this is not an optimization we
    // support.
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$elemMatch: {$ne: 5}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{a: {$elemMatch: {$not: {$gt: 3, $lt: 5}}}}"));
    assertHasOnlyCollscan();
}

//
// Hinting with all paths index tests.
//

TEST_F(QueryPlannerWildcardTest, ChooseWildcardIndexHint) {
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"), BSON("$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ChooseWildcardIndexHintByName) {
    std::string wildcard = "wildcard";
    CollatorInterface* nullCollator = nullptr;
    addWildcardIndex(BSON("$**" << 1), {}, {}, {}, nullCollator, wildcard);
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"),
                 BSON("$hint"
                      << "wildcard"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ChooseWildcardIndexHintWithPath) {
    addWildcardIndex(BSON("x.$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHint(fromjson("{x: {$eq: 1}}"), BSON("x.$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ChooseWildcardIndexHintWithOr) {
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1 << "y" << 1));

    runQueryHint(fromjson("{$or: [{x: 1}, {y: 1}]}"), BSON("$**" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: [{ixscan: {pattern: {$_path: 1, x: 1}}},"
        " {ixscan: {pattern: {$_path: 1, y: 1}}}]}}}}");
}

TEST_F(QueryPlannerWildcardTest, ChooseWildcardIndexHintWithCompoundIndex) {
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1 << "y" << 1));

    runQueryHint(fromjson("{x: 1, y: 1}"), BSON("$**" << 1));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, y: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, QueryNotInWildcardIndexHint) {
    addWildcardIndex(BSON("a.$**" << 1));
    addIndex(BSON("x" << 1));

    runInvalidQueryHint(fromjson("{x: {$eq: 1}}"), BSON("a.$**" << 1));
    assertNoSolutions();
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotExist) {
    addIndex(BSON("x" << 1));

    runInvalidQueryHint(fromjson("{x: {$eq: 1}}"), BSON("$**" << 1));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexHintWithPartialFilter) {
    auto filterObj = fromjson("{a: {$gt: 100}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, {}, filterExpr.get());

    runInvalidQueryHint(fromjson("{a: {$eq: 1}}"), BSON("$**" << 1));
    assertNoSolutions();
}

TEST_F(QueryPlannerWildcardTest, MultipleWildcardIndexesHintWithPartialFilter) {
    auto filterObj = fromjson("{a: {$gt: 100}, b: {$gt: 100}}");
    auto filterExpr = QueryPlannerTest::parseMatchExpression(filterObj);
    addWildcardIndex(BSON("$**" << 1), {}, {}, filterExpr.get());

    runInvalidQueryHint(fromjson("{a: {$eq: 1}, b: {$eq: 1}}"), BSON("$**" << 1));
    assertNoSolutions();
}

//
// Object comparison tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexesDoNotSupportObjectEquality) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {abc: 1}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$or: [{z: {abc: 1}}]}"));
    assertHasOnlyCollscan();

    // We can only use the index for the predicate on 'x'.
    runQuery(fromjson("{x: 5, y: {abc: 1}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexesDoNotSupportObjectInequality) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lt: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$lte: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gte: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gt: {abc: 1}}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {ne: {abc: 1}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$lt: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$lte: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gte: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$gt: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {ne: [1, 2, 'a string']}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$or: [{z: {$ne: {abc: 1}}}]}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{$and: [{x: 5}, {$or: [{x: 1}, {y: {abc: 1}}]}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

//
// Unsupported values tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexesDoNotSupportInWithUnsupportedValues) {
    addWildcardIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$in: [1, 2, 3, {abc: 1}]}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$in: [1, 2, 3, ['a', 'b', 'c']]}}"));
    assertHasOnlyCollscan();
    runQuery(fromjson("{x: {$in: [1, 2, 3, null]}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexesSupportElemMatchWithNull) {
    addWildcardIndex(BSON("$**" << 1));

    // Simple case.
    runQuery(fromjson("{x: {$elemMatch: {$lt: 5, $gt: 0}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");

    // null inside an $in inside an $elemMatch is supported by the wildcard index, since it means
    // we're searching for an explicit null value.
    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, null]}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexesDoNotSupportElemMatchWithUnsupportedValues) {
    runQuery(fromjson("{x: {$elemMatch: {$eq: ['a', 'b', 'c']}}}"));
    assertHasOnlyCollscan();

    // An object or array inside an $in inside a $elemMatch is not supported by the index.
    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, {a: 1}]}}}"));
    assertHasOnlyCollscan();

    runQuery(fromjson("{x: {$elemMatch: {$in: [1, 2, 3, ['a', 'b', 'c']]}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexesDoNotSupportElemMatchObject) {
    runQuery(fromjson("{x: {$elemMatch: {a: 1}}}"));
    assertHasOnlyCollscan();
}

//
// Sorting tests.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexCanProvideNonBlockingSort) {
    addWildcardIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: 1}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1}, "
        "bounds: {'$_path': [['a','a',true,true]], a: [[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       WildcardIndexCanProvideNonBlockingSortWhenFilterIncludesAdditionalFields) {
    addWildcardIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$gte: 3}, b: 1}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(2U);
    // The non-blocking sort solution.
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {'$_path': 1, a: 1}, "
        "bounds: {'$_path': [['a','a',true,true]], a: [[3,Infinity,true,true]]}}}}}");

    // A blocking sort solution (by doing a scan with a filter on 'b').
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node:"
        "{fetch: {filter: {a: {$gte: 3}}, node: "
        "{ixscan: {pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[1, 1, true, true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWithElemMatch) {
    addWildcardIndex(BSON("$**" << 1), {"a"});

    runQuerySortProj(fromjson("{a: {$elemMatch: {$eq: 1}}}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: {a: {$elemMatch: {$eq: 1}}}, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[1, 1, true, true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWithCompoundSort) {
    addWildcardIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$lte: 3}}"), BSON("a" << 1 << "b" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity, 3, true, true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWithExistsQueries) {
    addWildcardIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{a: {$exists: true}}"), BSON("a" << 1), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: null, node: "
        "{ixscan: {pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true], ['a.', 'a/', true, false]],"
        "a: [['MinKey', 'MaxKey', true, true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWhenFilterNotPresent) {
    // Since there's no filter on the field that we're sorting by, we cannot use an index scan to
    // answer the query as $** indexes are sparse.
    runQuerySortProj(BSONObj(), fromjson("{a: 1}"), BSONObj());
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWhenFilterDoesNotIncludeSortKey) {
    addWildcardIndex(BSON("$**" << 1));

    runQuerySortProj(fromjson("{b: 1, c: 1}"), fromjson("{a: 1}"), BSONObj());
    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: {c: 1}, node: "
        "{ixscan: {pattern: {'$_path': 1, b: 1},"
        "bounds: {'$_path': [['b','b',true,true]], b: [[1, 1, true, true]]}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, type: 'simple', node: "
        "{fetch: {filter: {b: 1}, node: "
        "{ixscan: {pattern: {'$_path': 1, c: 1},"
        "bounds: {'$_path': [['c','c',true,true]], c: [[1, 1, true, true]]}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexMustUseBlockingSortWhenFieldIsNotIncluded) {
    addWildcardIndex(BSON("$**" << 1), {}, BSON("b" << 0));

    runQuerySortProj(fromjson("{b: 1}"), fromjson("{b: 1}"), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {dir: 1, filter: {b: 1}}}}}");
}

//
// Field name or array index tests.
//

TEST_F(QueryPlannerWildcardTest, CannotAnswerQueryThroughArrayIndexProjection) {
    addWildcardIndex(BSON("$**" << 1), {"a"}, fromjson("{'a.0': 1, 'a.b': 1}"));

    // Queries whose path lies along a projected array index cannot be answered.
    runQuery(fromjson("{'a.0': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    runQuery(fromjson("{'a.0.b': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerQueryOnNonProjectedArrayIndex) {
    addWildcardIndex(BSON("$**" << 1), {"a", "c"}, fromjson("{'a.0': 1, b: 1, c: 1}"));

    // Queries on fields that are not projected array indices can be answered, even when such a
    // projection is present in the 'starPathsTempName' spec.
    runQuery(fromjson("{b: 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, b: 1}, "
        "bounds: {'$_path': [['b','b',true,true]], b: [[10,10,true,true]]}}}}}");

    // Queries on indices of arrays that have been projected in their entirety can be answered.
    runQuery(fromjson("{'c.0': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'c.0': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'c.0': "
        "1}, bounds: {'$_path': [['c','c',true,true], ['c.0','c.0',true,true]], 'c.0': "
        "[[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldGenerateAllPotentialPathBoundsForArrayIndexQueries) {
    addWildcardIndex(BSON("a.$**" << 1), {"a", "a.b"});

    // A numeric path component immediately following a multikey path may represent an array index,
    // a fieldname, or both. The $** index does not record array indices explicitly, so for all such
    // potential array indices in the query path, we generate bounds that both include and exclude
    // the numeric path components.
    runQuery(fromjson("{'a.0': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.0': "
        "1}, bounds: {'$_path': [['a','a',true,true], ['a.0','a.0',true,true]], 'a.0': "
        "[[10,10,true,true]]}}}}}");

    runQuery(fromjson("{'a.0.b': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.0.b': 1}, bounds: {'$_path': [['a.0.b','a.0.b',true,true], ['a.b','a.b',true,true]], "
        "'a.0.b': [[10,10,true,true]]}}}}}");

    runQuery(fromjson("{'a.0.b.1.c': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.0.b.1.c': 1}, bounds: {'$_path': [['a.0.b.1.c','a.0.b.1.c',true,true], "
        "['a.0.b.c','a.0.b.c',true,true], ['a.b.1.c','a.b.1.c',true,true], "
        "['a.b.c','a.b.c',true,true]], 'a.0.b.1.c': [[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldAddAllPredicatesToFetchFilterForArrayIndexQueries) {
    addWildcardIndex(BSON("$**" << 1), {"a", "a.b"});

    // Ordinarily, a query such as {'a': {$gt: 0, $lt: 5}} on a multikey field 'a' produces an EXACT
    // IXSCAN on one of the predicates and a FETCH filter on the other. For fieldname-or-array-index
    // queries on 'a.0' or similar, however, *all* predicates must be added to the FETCH filter to
    // ensure correctness.
    runQuery(fromjson("{'a.0': {$gt: 10, $lt: 20}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0': {$gt: 10, $lt: 20}}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'a.0': 1}, bounds: {'$_path': [['a','a',true,true], "
        "['a.0','a.0',true,true]], 'a.0': [[-inf,20,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.0': {$gt: 10, $lt: 20}}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'a.0': 1}, bounds: {'$_path': [['a','a',true,true], "
        "['a.0','a.0',true,true]], 'a.0': [[10,inf,false,true]]}}}}}");

    runQuery(fromjson("{'a.0.b': {$gte: 10, $lte: 20, $ne: 15}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b': {$gte: 10, $lte: 20, $ne: 15}}, node: {ixscan: {filter: null, "
        "pattern: {'$_path': 1, 'a.0.b': 1}, bounds: {'$_path': [['a.0.b','a.0.b',true,true], "
        "['a.b','a.b',true,true]], 'a.0.b': [[-inf,20.0,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b': {$gte: 10, $lte: 20, $ne: 15}}, node: {ixscan: {filter: null, "
        "pattern: {'$_path': 1, 'a.0.b': 1}, bounds: {'$_path': [['a.0.b','a.0.b',true,true], "
        "['a.b','a.b',true,true]], 'a.0.b': [[10,inf,true,true]]}}}}}");

    runQuery(fromjson("{'a.0.b.1.c': {$in: [10, 20, 30]}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': {$in: [10, 20, 30]}}, node: {ixscan: {filter: null, "
        "pattern: {'$_path': 1, 'a.0.b.1.c': 1}, bounds: {'$_path': "
        "[['a.0.b.1.c','a.0.b.1.c',true,true], ['a.0.b.c','a.0.b.c',true,true], "
        "['a.b.1.c','a.b.1.c',true,true], ['a.b.c','a.b.c',true,true]], 'a.0.b.1.c': "
        "[[10,10,true,true], [20,20,true,true], [30,30,true,true]]}}}}}");

    runQuery(fromjson("{'a.0.b.1.c': {$gte: 10, $lte: 30}, 'a.1.b.0.c': {$in: [10, 20, 30]}}"));
    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': {$gte: 10, $lte: 30}, 'a.1.b.0.c': {$in: [10, 20, 30]}}, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.0.b.1.c': 1}, bounds: {'$_path': "
        "[['a.0.b.1.c','a.0.b.1.c',true,true], ['a.0.b.c','a.0.b.c',true,true], "
        "['a.b.1.c','a.b.1.c',true,true], ['a.b.c','a.b.c',true,true]], 'a.0.b.1.c': "
        "[[-inf,30,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': {$gte: 10, $lte: 30}, 'a.1.b.0.c': {$in: [10, 20, 30]}}, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.0.b.1.c': 1}, bounds: {'$_path': "
        "[['a.0.b.1.c','a.0.b.1.c',true,true], ['a.0.b.c','a.0.b.c',true,true], "
        "['a.b.1.c','a.b.1.c',true,true], ['a.b.c','a.b.c',true,true]], 'a.0.b.1.c': "
        "[[10,inf,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': {$gte: 10, $lte: 30}, 'a.1.b.0.c': {$in: [10, 20, 30]}}, "
        "node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.1.b.0.c': 1}, bounds: {'$_path': "
        "[['a.1.b.0.c','a.1.b.0.c',true,true], ['a.1.b.c','a.1.b.c',true,true], "
        "['a.b.0.c','a.b.0.c',true,true], ['a.b.c','a.b.c',true,true]], 'a.1.b.0.c': "
        "[[10,10,true,true], [20,20,true,true], [30,30,true,true]]}}}}}");

    // Finally, confirm that queries which include a numeric path component on a field that is *not*
    // multikey produce EXACT bounds and do *not* add their predicates to the filter.
    runQuery(fromjson("{'d.0': {$gt: 10, $lt: 20}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'd.0': 1}, "
        "bounds: {'$_path': [['d.0','d.0',true,true]], 'd.0': [[10,20,false,false]]}}}}}");

    // Here, in contrast to the multikey case, we generate one indexed solution for each predicate
    // and only filter the *other* predicate in the FETCH stage.
    runQuery(fromjson("{'d.0.e': {$gt: 10, $lt: 20}, 'd.1.f': {$in: [10, 20, 30]}}"));
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {'d.0.e': {$gt: 10, $lt: 20}}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'd.1.f': 1}, bounds: {'$_path': [['d.1.f','d.1.f',true,true]], 'd.1.f': "
        "[[10,10,true,true], [20,20,true,true], [30,30,true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'d.1.f': {$in: [10, 20, 30]}}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'd.0.e': 1}, bounds: {'$_path': [['d.0.e','d.0.e',true,true]], 'd.0.e': "
        "[[10,20,false,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldOnlyBuildSpecialBoundsForMultikeyPaths) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.b", "a.b.c.d"});

    // 'a' is not multikey, so 'a.0' refers specifically to a fieldname rather than an array index,
    // and special bounds will not be generated.
    runQuery(fromjson("{'a.0': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.0': 1}, "
        "bounds: {'$_path': [['a.0','a.0',true,true]], 'a.0': [[10,10,true,true]]}}}}}");

    // 'a' is not multikey, so 'a.0.b' does not refer to multikey path 'a.b' and 'b.1' is therefore
    // also strictly a fieldname. Special bounds will not be generated for either.
    runQuery(fromjson("{'a.0.b.1': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.0.b.1': "
        "1}, bounds: {'$_path': [['a.0.b.1','a.0.b.1',true,true]], 'a.0.b.1': "
        "[[10,10,true,true]]}}}}}");

    // 'a.b' is multikey but 'a.b.c' is not, so special bounds will only be generated for 'b.1'.
    runQuery(fromjson("{'a.b.1.c.2.d': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.1.c.2.d': 10}, node: {ixscan: {filter: null, pattern: {'$_path': "
        "1, 'a.b.1.c.2.d': 1}, bounds: {'$_path': [['a.b.1.c.2.d','a.b.1.c.2.d',true,true], "
        "['a.b.c.2.d','a.b.c.2.d',true,true]], 'a.b.1.c.2.d': [[10,10,true,true]]}}}}}");

    // 'a.b' is multikey but 'a.b.c' is not. 'a.b.c.2.d' therefore refers to a different path than
    // multikey path 'a.[b].c.[d]', so special bounds will be generated for 'b.1' but not for 'd.3'.
    runQuery(fromjson("{'a.b.1.c.2.d.3.e': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.1.c.2.d.3.e': 10}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'a.b.1.c.2.d.3.e': 1}, bounds: "
        "{'$_path':[['a.b.1.c.2.d.3.e','a.b.1.c.2.d.3.e',true,true], "
        "['a.b.c.2.d.3.e','a.b.c.2.d.3.e',true,true]], 'a.b.1.c.2.d.3.e':[[10,10,true,true]]}}}}}");

    // 'a.b' and 'a.b.c.d' are both multikey. Special bounds will be generated for 'b.1' and 'd.3'.
    runQuery(fromjson("{'a.b.1.c.d.3.e': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.1.c.d.3.e': 10}, node: {ixscan: {filter: null, pattern: {'$_path': "
        "1, 'a.b.1.c.d.3.e': 1}, bounds: {'$_path':[['a.b.1.c.d.3.e','a.b.1.c.d.3.e',true,true], "
        "['a.b.1.c.d.e','a.b.1.c.d.e',true,true], ['a.b.c.d.3.e','a.b.c.d.3.e',true,true], "
        "['a.b.c.d.e','a.b.c.d.e',true,true]], 'a.b.1.c.d.3.e':[[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldGenerateSpecialBoundsForNumericMultikeyPaths) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.b", "a.b.0", "a.b.0.c"});

    // 'a.b.0' is itself a multikey path, but since 'a.b' is multikey 'b.0' may refer to an array
    // element of 'b'. We generate special bounds for 'b.0'.
    runQuery(fromjson("{'a.b.0': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.0': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.b.0': 1}, bounds: {'$_path': [['a.b','a.b',true,true], ['a.b.0','a.b.0',true,true]], "
        "'a.b.0': [[10,10,true,true]]}}}}}");

    // 'a.b' and 'a.b.0.c' are both multikey paths; we generate special bounds for 'b.0' and 'c.1'.
    runQuery(fromjson("{'a.b.0.c.1.d': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.0.c.1.d': 10}, node: {ixscan: {filter: null, pattern: {'$_path': "
        "1, 'a.b.0.c.1.d': 1}, bounds: {'$_path': [['a.b.0.c.1.d','a.b.0.c.1.d',true,true], "
        "['a.b.0.c.d','a.b.0.c.d',true,true], ['a.b.c.1.d','a.b.c.1.d',true,true], "
        "['a.b.c.d','a.b.c.d',true,true]], 'a.b.0.c.1.d': [[10,10,true,true]]}}}}}");

    // 'a.b' is multikey but 'a.b.c' is not, so we only generate special bounds for 'a.b.1'.
    runQuery(fromjson("{'a.b.1.c.1': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.1.c.1': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'a.b.1.c.1': 1}, bounds: {'$_path': [['a.b.1.c.1','a.b.1.c.1',true,true], "
        "['a.b.c.1','a.b.c.1',true,true]], 'a.b.1.c.1': [[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldNotGenerateSpecialBoundsForFieldNamesWithLeadingZeroes) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.b"});

    // 'a.b' is multikey, but '01' is not eligible for consideration as a numeric array index; it is
    // always strictly a field name. We therefore do not generate special bounds.
    runQuery(fromjson("{'a.b.01': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, 'a.b.01': 1}, "
        "bounds: {'$_path': [['a.b.01','a.b.01',true,true]], 'a.b.01': [[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, InitialNumericPathComponentIsAlwaysFieldName) {
    addWildcardIndex(BSON("$**" << 1), {"0"}, fromjson("{'0': 1}"));

    // '0' is multikey, but the first component in a path can never be an array index since it must
    // always be a field name. We therefore do not generate special bounds for '0.b'.
    runQuery(fromjson("{'0.b': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, '0.b': 1}, "
        "bounds: {'$_path': [['0.b','0.b',true,true]], '0.b': [[10,10,true,true]]}}}}}");

    // '0' is multikey, so a query on '0.1.b' will generate special bounds for '1' but not for '0'.
    runQuery(fromjson("{'0.1.b': 10}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'0.1.b': 10}, node: {ixscan: {filter: null, pattern: {'$_path': 1, "
        "'0.1.b': 1}, bounds: {'$_path': [['0.1.b','0.1.b',true,true], ['0.b','0.b',true,true]], "
        "'0.1.b': [[10,10,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldGenerateSpecialBoundsForNullAndExistenceQueries) {
    addWildcardIndex(BSON("a.$**" << 1),
                     {"a", "a.b", "a.b.2", "a.b.2.3", "a.b.2.3.4", "a.c.b", "a.c.b.1"});

    runQuery(fromjson("{'a.0.b': {$exists: true}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b': {$exists: true}}, node: {ixscan: {filter: null, pattern: "
        "{'$_path': 1, 'a.0.b': 1}, bounds: {'$_path': [['a.0.b','a.0.b',true,true], "
        "['a.0.b.','a.0.b/',true,false], ['a.b','a.b',true,true], ['a.b.','a.b/',true,false]], "
        "'a.0.b': [[{$minKey: 1},{$maxKey: 1},true,true]]}}}}}");

    runQuery(fromjson("{'a.0.b.1.c': {$exists: true, $eq: null}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.b.1.c': {$exists: true, $eq: null}}, node: {ixscan: {filter:null, "
        "pattern:{'$_path': 1, 'a.0.b.1.c': 1}, bounds: {'$_path': "
        "[['a.0.b.1.c','a.0.b.1.c',true,true], ['a.0.b.1.c.','a.0.b.1.c/',true,false], "
        "['a.0.b.c','a.0.b.c',true,true], ['a.0.b.c.','a.0.b.c/',true,false], "
        "['a.b.1.c','a.b.1.c',true,true], ['a.b.1.c.','a.b.1.c/',true,false], "
        "['a.b.c','a.b.c',true,true], ['a.b.c.','a.b.c/',true,false]], 'a.0.b.1.c': [[{$minKey: "
        "1},{$maxKey: 1},true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       CanAnswerMultipleNumericPathComponentsWhenPrefixPathIsNotMultikey) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{'a.0.1': 'foo'}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, 'a.0.1': 1}, bounds:"
        "{$_path: [['a.0.1','a.0.1',true,true]], 'a.0.1': [['foo','foo',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       CannotAnswerMultipleNumericPathComponentsWhenPrefixPathIsMultikey) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{'a.0.1': 'foo'}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest,
       CannotAnswerMultipleNumericPathComponentsWhenPrefixMultikeyPathIsNumeric) {
    addWildcardIndex(BSON("$**" << 1), {"a.0"});
    runQuery(fromjson("{'a.0.1.2': 'foo'}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, CannotAnswerExistencePredicateOnMultipleNumericPathComponents) {
    addWildcardIndex(BSON("a.$**" << 1), {"a"});

    // This query cannot be serviced by the wildcard index, because there are multiple successive
    // positional path components.
    runQuery(fromjson("{'a.0.0.0': {$exists: true}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, CannotAnswerNullEqualityPredicateOnMultipleNumericPathComponents) {
    addWildcardIndex(BSON("a.$**" << 1),
                     {"a", "a.b", "a.b.2", "a.b.2.3", "a.b.2.3.4", "a.c.b", "a.c.b.1"});

    runQuery(fromjson("{'a.0.b.2.3.4': {$exists: true, $eq: null}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest,
       CannotAnswerEqualityPredicateWithMultipleNumericPathComponentsInMiddleOfPath) {
    addWildcardIndex(BSON("a.$**" << 1), {"a"});

    runQuery(fromjson("{'a.10.11.b.2': {$eq: 'foo'}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, DoNotProduceOverlappingBoundsWithMultipleNumericPathComponents) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.0"});
    // When an array index field exists in the query pattern and one of the resulting
    // fieldname-or-array-index paths is a prefix of another, then the subpath bounds generated by
    // the prefix path will contain all the bounds generated by its subpaths. Test that we avoid
    // overlap by removing the redundant paths.
    //
    // In the below example, 'a.0' is multikey and the query is on 'a.0.0'. We generate paths
    // 'a.0' and 'a.0.0' because the first '0' is an array index. But the subpaths bound
    // generated by 'a.0' -> ['a.0.','a.0/'] would contain all the bounds generated by
    // 'a.0.0'. Therefore we must remove path 'a.0.0' before generating the subpath bounds.
    runQuery(fromjson("{'a.0.0': {$exists: true}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.0': {$exists: true}}, node: {ixscan: {filter:null, "
        "pattern:{'$_path': 1, 'a.0.0': 1}, bounds: {'$_path': [['a.0','a.0',true,true], "
        "['a.0.','a.0/',true,false]], 'a.0.0': [[{$minKey: 1},{$maxKey: 1},true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       DoNotProduceOverlappingBoundsWithMultipleNumericPathComponentsInMiddleOfPath) {
    addWildcardIndex(BSON("a.$**" << 1), {"a.0", "a.0.b"});
    runQuery(fromjson("{'a.0.0.b.1': {$exists: true}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.0.0.b.1': {$exists: true}}, node: {ixscan: {filter:null, "
        "pattern:{'$_path': 1, 'a.0.0.b.1': 1}, bounds: {'$_path': "
        "[['a.0.0.b','a.0.0.b',true,true], "
        "['a.0.0.b.','a.0.0.b/',true,false], ['a.0.b','a.0.b',true,true], "
        "['a.0.b.','a.0.b/',true,false]],"
        "'a.0.0.b.1': [[{$minKey: 1},{$maxKey: 1},true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ShouldDeclineToAnswerQueriesThatTraverseTooManyArrays) {
    addWildcardIndex(BSON("$**" << 1),
                     {"a",
                      "a.b",
                      "a.b.c",
                      "a.b.c.d",
                      "a.b.c.d.e",
                      "a.b.c.d.e.f",
                      "a.b.c.d.e.f.g",
                      "a.b.c.d.e.f.g.h",
                      "a.b.c.d.e.f.g.h.i",
                      "a.b.c.d.e.f.g.h.i.j",
                      "a.b.c.d.e.f.g.h.i.j.k"});

    // We can query through 8 levels of arrays via explicit array indices...
    runQuery(fromjson("{'a.1.b.2.c.3.d.4.e.5.f.6.g.7.h.8': 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {$_path: 1, 'a.1.b.2.c.3.d.4.e.5.f.6.g.7.h.8': 1}}}}}");

    // ... but after that, we decline to answer the query using the $** index.
    runQuery(fromjson("{'a.1.b.2.c.3.d.4.e.5.f.6.g.7.h.8.i.9': 1}"));
    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");

    // However, we continue to allow implicit (non-indices) traversal of arrays to any depth.
    runQuery(fromjson("{'a.1.b.2.c.3.d.4.e.5.f.6.g.7.h.8.i.j.k': 1}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch:{node:{ixscan:{pattern:{$_path:1, 'a.1.b.2.c.3.d.4.e.5.f.6.g.7.h.8.i.j.k': 1}}}}}");
}

//
// Min/max with wildcard index.
//

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMin) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(BSONObj(), BSON("$**" << 1), fromjson("{x: 1}"), BSONObj());
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMax) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(BSONObj(), BSON("$**" << 1), BSONObj(), fromjson("{x: 10}"));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMinMax) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(BSONObj(), BSON("$**" << 1), fromjson("{x: 1}"), fromjson("{x: 10}"));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportCompoundMinMax) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(
        BSONObj(), BSON("$**" << 1), fromjson("{x: 1, y: 1}"), fromjson("{x: 10, y:10}"));
}

// Test with a query argument to check that the expanded indices are not used.

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMinWithFilter) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(
        fromjson("{x: {$eq: 5}}"), BSON("$**" << 1), fromjson("{x: 1}"), BSONObj());
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMaxWithFilter) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(
        fromjson("{x: {$eq: 5}}"), BSON("$**" << 1), BSONObj(), fromjson("{x: 10}"));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportMinMaxWithFilter) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(
        fromjson("{x: {$eq: 5}}"), BSON("$**" << 1), fromjson("{x: 1}"), fromjson("{x: 10}"));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotSupportCompoundMinMaxWithFilter) {
    addWildcardIndex(BSON("$**" << 1));

    runInvalidQueryHintMinMax(fromjson("{x: 5, y: 5}"),
                              BSON("$**" << 1),
                              fromjson("{x: 1, y: 1}"),
                              fromjson("{x: 10, y:10}"));
}

TEST_F(QueryPlannerWildcardTest, PathSpecifiedWildcardIndexDoesNotSupportMinMax) {
    addWildcardIndex(BSON("x.$**" << 1));

    runInvalidQueryHintMinMax(
        fromjson("{x: {$eq: 5}}"), BSON("x.$**" << 1), fromjson("{x: 1}"), fromjson("{x: 10}"));
}

TEST_F(QueryPlannerWildcardTest, WildcardIndexDoesNotAffectValidMinMax) {
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("x" << 1));

    runQueryHintMinMax(fromjson("{x: {$eq: 5}}"),
                       BSONObj(fromjson("{x: 1}")),
                       fromjson("{x: 1}"),
                       fromjson("{x: 10}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {x: {$eq: 5}}, "
        "node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObject) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a', 'a', true, true]], a: [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerLTEToEmptyObject) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$lte: {}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a', 'a', true, true]], a: [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CannotAnswerInequalityToEmptyObject) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$gte: {}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, CannotAnswerNotEqualToEmptyObject) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$ne: {}}}"));
    assertHasOnlyCollscan();
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObjectWhenPathIsMultikey) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$eq: {}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a', 'a', true, true]], a: [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObjectWithProjectionInKeyPattern) {
    addWildcardIndex(BSON("a.b.$**" << 1));
    runQuery(fromjson("{'a.b.c': {$eq: {}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, 'a.b.c': 1}},"
        "bounds: {$_path: [['a.b.c', 'a.b.c', true, true]], 'a.b.c': [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObjectWithWildcardProjection) {
    addWildcardIndex(BSON("$**" << 1), {}, BSON("a.b" << 1 << "c.d" << 1));
    runQuery(fromjson("{'a.b.c': {$eq: {}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, 'a.b.c': 1}},"
        "bounds: {$_path: [['a.b.c', 'a.b.c', true, true]], 'a.b.c': [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObjectWithinElemMatchObject) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {b: {}}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$eq: {}}}}},"
        "node: {ixscan: {filter: null, pattern: {$_path: 1, 'a.b': 1}},"
        "bounds: {$_path: [['a.b', 'a.b', true, true]], 'a.b': [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerEqualityToEmptyObjectWithinElemMatchValue) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$eq: {}}}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$eq: {}}}},"
        "node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a', 'a', true, true]], a: [[{}, {}, true, true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerInContainingEmptyObject) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$in: [2, {}]}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a','a',true,true]], a: [[2,2,true,true],[{},{},true,true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanAnswerInContainingEmptyObjectWhenPathIsMultikey) {
    addWildcardIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$in: [2, {}]}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {$_path: 1, a: 1}},"
        "bounds: {$_path: [['a','a',true,true]], a: [[2,2,true,true],[{},{},true,true]]}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanProduceSortMergePlanWithWildcardIndex) {
    auto defaultMaxOr = internalQueryEnumerationMaxOrSolutions.load();
    ON_BLOCK_EXIT([&] { internalQueryEnumerationMaxOrSolutions.store(defaultMaxOr); });
    internalQueryEnumerationMaxOrSolutions.store(5);
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 1, b: 1}, {b: 2}]}, sort: {b: -1}}"));
    assertNumSolutions(3U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{fetch: {filter: {a: 1}, node: {ixscan: {pattern: {$_path: 1, b: 1}, bounds: "
        "{$_path: [['b','b',true,true]], b: [[1,1,true,true]]}}}}},"
        "{ixscan: {pattern: {$_path: 1, b: 1}, bounds:"
        "{$_path: [['b','b',true,true]], b: [[2,2,true,true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {mergeSort: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: "
        "{a: [[1,1,true,true]], b: [[1,1,true,true]]}}},"
        "{ixscan: {pattern: {$_path: 1, b: 1}, bounds:"
        "{$_path: [['b','b',true,true]], b: [[2,2,true,true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {sort: {pattern: {b: -1}, type: 'default', limit: 0, "
        "node: {or: {nodes: ["
        "{fetch: {filter: {b: 1}, node: {ixscan: {pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true]], a: [[1,1,true,true]]}}}}},"
        "{ixscan: {filter: null, pattern: {$_path: 1, b: 1}, bounds:"
        "{$_path: [['b','b',true,true]], b: [[2,2,true,true]]}}}]}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, ContainedOrPushdownWorksWithWildcardIndex) {
    auto defaultMaxOr = internalQueryEnumerationMaxOrSolutions.load();
    ON_BLOCK_EXIT([&] { internalQueryEnumerationMaxOrSolutions.store(defaultMaxOr); });
    internalQueryEnumerationMaxOrSolutions.store(5);
    addWildcardIndex(BSON("$**" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    runQuery(fromjson("{a: 1, $or: [{c: 2}, {b: 3}]}"));
    assertNumSolutions(4U);
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, filter: null, bounds:"
        "{a: [[1,1,true,true]], b: [[3,3,true,true]]}}},"
        "{ixscan: {pattern: {$_path: 1, c: 1}, filter: null, bounds:"
        "{$_path: [['c','c',true,true]], c: [[2,2,true,true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: 1}, node: {or: {nodes: ["
        "{ixscan: {pattern: {$_path: 1, c: 1}, filter: null, bounds:"
        "{$_path: [['c','c',true,true]], c: [[2,2,true,true]]}}},"
        "{ixscan: {pattern: {$_path: 1, b: 1}, filter: null, bounds:"
        "{$_path: [['b','b',true,true]], b: [[3,3,true,true]]}}}]}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{c: 2}, {b: 3}]}, node:"
        "{ixscan: {filter: null, pattern: {a: 1, b: 1}, bounds:"
        "{a: [[1,1,true,true]], b:[['MinKey','MaxKey',true,true]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {$or: [{c: 2}, {b: 3}]}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true]], a:[[1,1,true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, TypeOfObjectWithWildcardIndex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$type: 'object'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$type: [3]}}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true], ['a.','a/', true, false]], "
        "a:[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, TypeOfArrayWithWildcardIndex) {
    addWildcardIndex(BSON("$**" << 1));
    runQuery(fromjson("{a: {$type: 'array'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$type: [4]}}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true], ['a.','a/', true, false]], "
        "a:[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       StringComparisonWithUnequalCollatorsAndWildcardIndexResultsInCollscan) {
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addWildcardIndex(fromjson("{'$**': 1}"), {}, {}, {}, &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, StringComparisonWithEqualCollatorsAndWildcardIndexUsesIndex) {
    params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;

    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    addWildcardIndex(fromjson("{'$**': 1}"), {}, {}, {}, &reverseStringCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: null, collation: {locale: 'reverse'}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], 'a': [['','oof',true,false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanUseWildcardIndexAndPushProjectionBeneathSort) {
    addWildcardIndex(BSON("$**" << 1));
    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: 1, b: {$gt: 0}}, projection: {_id: 0, b: 1}, sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, b: 1}, node: {fetch: {filter: {a: 1}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, b: 1}, bounds: "
        "{$_path: [['b','b',true,true]], b: [[0,Infinity,false,true]]}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node:"
        "{proj: {spec: {_id: 0, b: 1}, node: {fetch: {filter: {b: {$gt: 0}}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true]], a: [[1,1,true,true]]}}}}}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CanPushProjectionBeneathSortWithExistsPredicate) {
    addWildcardIndex(BSON("$**" << 1));
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: {$exists: true}}, projection: {_id: 0, b: 1}, "
                 "sort: {b: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0,  type: 'simple', node:"
        "{proj: {spec: {_id: 0, b: 1}, node: {fetch: {filter: {a: {$eq: 1}}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, b: 1}, bounds:"
        "{$_path: [['b','b',true,true], ['b.', 'b/', true, false]],"
        "b: [['MinKey','MaxKey',true,true]]}}}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node:"
        "{proj: {spec: {_id: 0, b: 1}, node: {fetch: {filter: {b: {$exists: true}}, node:"
        "{ixscan: {filter: null, pattern: {$_path: 1, a: 1}, bounds:"
        "{$_path: [['a','a',true,true]], a: [[1,1,true,true]]}}}}}}}}}");
}

}  // namespace mongo
