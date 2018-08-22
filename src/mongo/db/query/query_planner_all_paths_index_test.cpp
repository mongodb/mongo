/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_planner_test_fixture.h"

namespace mongo {

const std::string kIndexName = "indexName";

/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner with
 * a view of the available $** indexes.
 */
class QueryPlannerAllPathsTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // We're interested in testing plans that use a $** index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void addAllPathsIndex(BSONObj keyPattern,
                          const std::set<std::string>& multikeyPathSet = {},
                          BSONObj starPathsTempName = BSONObj{}) {
        // Convert the set of std::string to a set of FieldRef.
        std::set<FieldRef> multikeyFieldRefs;
        for (auto&& path : multikeyPathSet) {
            ASSERT_TRUE(multikeyFieldRefs.emplace(path).second);
        }
        ASSERT_EQ(multikeyPathSet.size(), multikeyFieldRefs.size());

        const bool isMultikey = !multikeyPathSet.empty();
        BSONObj infoObj = BSON("starPathsTempName" << starPathsTempName);

        params.indices.push_back(IndexEntry{std::move(keyPattern),
                                            IndexType::INDEX_ALLPATHS,
                                            isMultikey,
                                            {},  // multikeyPaths
                                            std::move(multikeyFieldRefs),
                                            false,  // sparse
                                            false,  // unique
                                            IndexEntry::Identifier{kIndexName},
                                            nullptr,  // partialFilterExpression
                                            std::move(infoObj),
                                            nullptr});  // collator
    }
};

//
// Null comparison and existence tests.
//

TEST_F(QueryPlannerAllPathsTest, ExistsTrueQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ExistsFalseQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, NotEqualsNullQueriesDontUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerAllPathsTest, NotEqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$ne: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EqualsNullAndExistsQueriesUseAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$eq: null, $exists: true}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, EmptyBoundsWithAllPathsIndexes) {
    addIndex(BSON("$**" << 1));

    runQuery(fromjson("{x: {$lte: 5, $gte: 10}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {$_path: 1, x: 1}}}}}");
}

//
// Multikey planning tests.
//

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverMultikeyFieldNoElemMatch) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverMultikeyFieldWithElemMatch) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {$gt: 0, $lt: 9}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {$gt: 0, $lt: 9}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNonMultikeyFieldWithMultikeyIndex) {
    addAllPathsIndex(BSON("$**" << 1), {"b"});
    runQuery(fromjson("{a: {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {'$_path': 1, a: 1},"
        "bounds: {'$_path': [['a','a',true,true]], a: [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithFirstComponentMultikey) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{'a.b': {$gt: 0, $lt: 9}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$lt: 9}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithElemMatchObject) {
    addAllPathsIndex(BSON("$**" << 1), {"a"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest,
       MultiplePredicatesOverNestedFieldWithElemMatchObjectBothComponentsMultikey) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[-Infinity,9,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, MultiplePredicatesOverNestedFieldWithTwoElemMatches) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b"});
    runQuery(fromjson("{a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {a: {$elemMatch: {b: {$elemMatch: {$gt: 0, $lt: 9}}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b': 1},"
        "bounds: {'$_path': [['a.b','a.b',true,true]], 'a.b': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, ElemMatchOnInnermostMultikeyPathPermitsTightBounds) {
    addAllPathsIndex(BSON("$**" << 1), {"a", "a.b", "a.b.c"});
    runQuery(fromjson("{'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}"));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b.c': {$elemMatch: {'d.e.f': {$gt: 0, $lt: 9}}}}, node: "
        "{ixscan: {filter: null, pattern: {'$_path': 1, 'a.b.c.d.e.f': 1},"
        "bounds: {'$_path': [['a.b.c.d.e.f','a.b.c.d.e.f',true,true]],"
        "'a.b.c.d.e.f': [[0,9,false,false]]}}}}}");
}

TEST_F(QueryPlannerAllPathsTest, AllPredsEligibleForIndexUseGenerateCandidatePlans) {
    addAllPathsIndex(BSON("a.$**" << 1), {"a.b", "a.c"});
    runQuery(
        fromjson("{'a.b': {$gt: 0, $lt: 9}, 'a.c': {$gt: 11, $lt: 20}, d: {$gt: 31, $lt: 40}}"));

    ASSERT_EQUALS(getNumSolutions(), 4U);
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

}  // namespace mongo
