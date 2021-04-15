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

/**
 * This file contains tests for mongo/db/query/planner_ixselect.cpp
 */

#include "mongo/db/query/index_entry.h"

#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/bson_test_util.h"

#include <algorithm>

namespace mongo {

using PlannerWildcardHelpersTest = AggregationContextFixture;

auto getLastElement(const BSONObj& keyPattern) {
    auto it = keyPattern.begin();
    while (std::next(it) != keyPattern.end()) {
        it++;
    }
    return it;
}

/**************** The following section can be moved to planner_ixselect_test.cpp ****************/
/*
 * Will compare 'keyPatterns' with 'entries'. As part of comparing, it will sort both of them.
 */
bool indexEntryKeyPatternsMatch(std::vector<BSONObj>* keyPatterns,
                                std::vector<IndexEntry>* entries) {
    ASSERT_EQ(entries->size(), keyPatterns->size());

    const auto cmpFn = [](const IndexEntry& a, const IndexEntry& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.keyPattern < b.keyPattern);
    };

    std::sort(entries->begin(), entries->end(), cmpFn);
    std::sort(keyPatterns->begin(), keyPatterns->end(), [](const BSONObj& a, const BSONObj& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a < b);
    });

    return std::equal(keyPatterns->begin(),
                      keyPatterns->end(),
                      entries->begin(),
                      [](const BSONObj& keyPattern, const IndexEntry& ie) -> bool {
                          return SimpleBSONObjComparator::kInstance.evaluate(keyPattern ==
                                                                             ie.keyPattern);
                      });
}

// Helper which constructs an IndexEntry and returns it along with an owned ProjectionExecutor,
// which is non-null if the requested entry represents a wildcard index and null otherwise. When
// non-null, it simulates the ProjectionExecutor that is owned by the $** IndexAccessMethod.
auto makeIndexEntry(BSONObj keyPattern,
                    MultikeyPaths multiKeyPaths,
                    std::set<FieldRef> multiKeyPathSet = {},
                    BSONObj infoObj = BSONObj()) {
    auto wcElem = getLastElement(keyPattern);
    auto wcProj = wcElem->fieldNameStringData().endsWith("$**"_sd)
        ? std::make_unique<WildcardProjection>(WildcardKeyGenerator::createProjectionExecutor(
              keyPattern, infoObj.getObjectField("wildcardProjection")))
        : std::unique_ptr<WildcardProjection>(nullptr);

    auto multiKey = !multiKeyPathSet.empty() ||
        std::any_of(multiKeyPaths.cbegin(), multiKeyPaths.cend(), [](const auto& entry) {
            return !entry.empty();
        });
    return std::make_pair(IndexEntry(keyPattern,
                                     IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                                     IndexDescriptor::kLatestIndexVersion,
                                     multiKey,
                                     multiKeyPaths,
                                     multiKeyPathSet,
                                     false,
                                     false,
                                     CoreIndexInfo::Identifier("test_foo"),
                                     nullptr,
                                     {},
                                     nullptr,
                                     wcProj.get()),
                          std::move(wcProj));
}

std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    ASSERT_TRUE(status.isOK());
    return std::unique_ptr<MatchExpression>(status.getValue().release());
}

TEST_F(PlannerWildcardHelpersTest, ExpandSimpleWildcardIndexEntry) {
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"a"};
    const auto indexEntry = makeIndexEntry(BSON("$**" << 1), {});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_BSONOBJ_EQ(out[0].keyPattern, fromjson("{a: 1}"));
}

TEST_F(PlannerWildcardHelpersTest, ExpandCompoundWildcardIndexEntry) {
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"a", "b", "c"};
    const auto indexEntry = makeIndexEntry(
        BSON("a" << 1 << "$**" << 1), {}, {}, {fromjson("{wildcardProjection: {a: 0}}")});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    ASSERT_EQ(out.size(), 2u);
    std::vector<BSONObj> expectedKeyPatterns = {fromjson("{a: 1, b: 1}"), fromjson("{a: 1, c: 1}")};
    indexEntryKeyPatternsMatch(&expectedKeyPatterns, &out);
}

TEST_F(PlannerWildcardHelpersTest, ExpandCompoundWildcardIndexEntryNoMatch) {
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"c", "b"};
    const auto indexEntry = makeIndexEntry(
        BSON("a" << 1 << "$**" << 1), {}, {}, {fromjson("{wildcardProjection: {a: 0}}")});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    ASSERT_EQ(out.size(), 2u);
    std::vector<BSONObj> expectedKeyPatterns = {fromjson("{a: 1, b: 1}"), fromjson("{a: 1, c: 1}")};
    indexEntryKeyPatternsMatch(&expectedKeyPatterns, &out);
}

TEST_F(PlannerWildcardHelpersTest, ExpandEnsureMultikeySetForAllCompoundFields) {
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"a", "b"};
    const auto indexEntry = makeIndexEntry(BSON("a" << 1 << "$**" << 1),
                                           {},
                                           {FieldRef("a"), FieldRef("b"), FieldRef("c")},
                                           {fromjson("{wildcardProjection: {a: 0}}")});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].multikey);
    ASSERT_EQ(out[0].multikeyPaths.size(), 2);
    ASSERT(out[0].multikeyPaths[0] == MultikeyComponents{0u});  // a is a multikey path
    ASSERT(out[0].multikeyPaths[1] == MultikeyComponents{0u});  // and so is b
    ASSERT_BSONOBJ_EQ(out[0].keyPattern, {fromjson("{a: 1, b: 1}")});
}

TEST_F(PlannerWildcardHelpersTest, ExpandEnsureMultikeySetForAllCompoundFieldsDotted) {
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"a.b", "c.d.e"};
    const auto indexEntry = makeIndexEntry(
        BSON("a.b" << 1 << "$**" << 1),
        {},
        {FieldRef("a"), FieldRef("a.b"), FieldRef("b"), FieldRef("c"), FieldRef("c.d.e")},
        {fromjson("{wildcardProjection: {a: 0}}")});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].multikey);
    ASSERT_EQ(out[0].multikeyPaths.size(), 2);
    ASSERT((out[0].multikeyPaths[0] == MultikeyComponents{0u, 1u}));  // a and a.b are multikey
    ASSERT((out[0].multikeyPaths[1] == MultikeyComponents{0u, 2u}));  // c and c.d.e are multikey
    ASSERT_BSONOBJ_EQ(out[0].keyPattern, {fromjson("{'a.b': 1, 'c.d.e': 1}")});
}

/*************************************** end section ***************************************/

// translateWildcardIndexBoundsAndTightness

TEST_F(PlannerWildcardHelpersTest, TranslateBoundsWithWildcard) {
    // expand first
    std::vector<IndexEntry> out;
    stdx::unordered_set<std::string> fields{"a", "b"};
    const auto indexEntry = makeIndexEntry(
        BSON("a" << 1 << "$**" << 1), {}, {}, {fromjson("{wildcardProjection: {a: 0}}")});
    wildcard_planning::expandWildcardIndexEntry(indexEntry.first, fields, &out);

    // This expression can only be over one field. WTS that given a query on a field and a compound
    // index on that field (followed by wildcard) that we translate properly.
    BSONObj obj = fromjson("{a: {$lte: 1}}");
    auto expr = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, out[0], &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

// How to test?
// finalizeWildcardIndexScanConfiguration(IndexScanNode* scan);
// isWildcardObjectSubpathScan(const IndexScanNode* node);


/********** The following section can be moved to query_planner_wildcard_index_test.cpp **********/
class QueryPlannerWildcardTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // We're interested in testing plans that use a $** index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void addWildcardIndex(BSONObj keyPattern,
                          const std::set<std::string>& multikeyPathSet = {},
                          BSONObj wildcardProjection = BSONObj{},
                          MatchExpression* partialFilterExpr = nullptr,
                          CollatorInterface* collator = nullptr,
                          const std::string& indexName = "indexName") {
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

TEST_F(QueryPlannerWildcardTest, CompoundWildcardIndexQueryOnlyOnNonWCFieldWithProjection) {
    addWildcardIndex(fromjson("{a: 1, '$**': 1}"), {}, fromjson("{a: 0}"));

    runQuery(fromjson("{a: {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, '$_path': 1, '$_value': 1}, bounds: {'a': "
        "[[5, 5, true, true]], '$_path': [['$_value', '$_value', true, true]], '$_value': "
        "[['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundWildcardIndexQueryOnlyOnNonWCField) {
    addWildcardIndex(fromjson("{a: 1, 'b.$**': 1}"), {});

    runQuery(fromjson("{a: {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, '$_path': 1, 'b.$_value': 1}, bounds: {'a': "
        "[[5, 5, true, true]], '$_path': [['b.$_value', 'b.$_value', true, true]], 'b.$_value': "
        "[['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundWildcardIndexQueryOnMultipleNonWCField) {
    addWildcardIndex(fromjson("{a: 1, x: 1, 'b.$**': 1}"), {});

    runQuery(fromjson("{a: {$eq: 5}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, x: 1, '$_path': 1, 'b.$_value': 1}, bounds: {'a':"
        "[[5, 5, true, true]], 'x': [['MinKey', 'MaxKey', true, true]], '$_path': [['b.$_value', "
        "'b.$_value', true, true]], 'b.$_value': [['MinKey', 'MaxKey', true, true]]}}}}}");

    runQuery(fromjson("{a: {$eq: 5}, x: {$lt: 2}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, x: 1, '$_path': 1, 'b.$_value': 1}, bounds: {'a':"
        "[[5, 5, true, true]], 'x': [[-Infinity, 2, true, false]], '$_path': [['b.$_value', "
        "'b.$_value', true, true]], 'b.$_value': [['MinKey', 'MaxKey', true, true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundWildcardIndexBasic) {
    addWildcardIndex(fromjson("{a: 1, '$**': 1}"), {}, fromjson("{a: 0}"));

    runQuery(fromjson("{a: {$eq: 5}, x: {$lt: 3}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, $_path: 1, x: 1}, bounds: {'a': "
        "[[5, 5, true, true]], '$_path': [['x', 'x', true, true]], 'x': [[-Infinity, 3, true, "
        "false]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundWildcardIndexIsNotUsedWhenQueryNotOnIndexPrefix) {
    addWildcardIndex(fromjson("{a: 1, '$**': 1}"), {}, fromjson("{a: 0}"));

    runQuery(fromjson("{x: {$lt: 3}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest,
       CompoundWildcardIndexIsNotUsedWhenQueryNotOnIndexPrefixAndNotIncludedInWildcard) {
    addWildcardIndex(fromjson("{a: 1, 'b.$**': 1}"), {});

    runQuery(fromjson("{c: {$eq: 5}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundEqualsNullQueryDoesUseWildcardIndexes) {
    addWildcardIndex(fromjson("{a: 1, '$**': 1}"), {}, fromjson("{a: 0}"));

    runQuery(fromjson("{a: {$lt: 2}, x: {$eq: null}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'x': {$eq: null}}, node: "
        "{ixscan: {filter: null, pattern: {'a': 1, '$_path': 1, 'x': 1},"
        "bounds: {'a': [[-Infinity, 2, true, false]], '$_path': [['x','x',true,true]], 'x': "
        "[['MinKey','MaxKey',true,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest, CompoundWildcardWithMultikeyField) {
    addWildcardIndex(
        fromjson("{a: 1, '$**': 1}"), {"b"} /* 'b' marked as multikey field */, fromjson("{a: 0}"));
    runQuery(fromjson("{a: {$eq: 5}, b: {$gt: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {a: 1, $_path: 1, b: 1}, bounds: {'a': "
        "[[5, 5, true, true]], '$_path': [['b','b',true,true]], b: "
        "[[0,Infinity,false,true]]}}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       CompoundWildcardMultiplePredicatesOverNestedFieldWithFirstComponentMultikey) {
    addWildcardIndex(fromjson("{x: 1, '$**': 1}"), {"a"}, fromjson("{x: 0}"));
    runQuery(fromjson("{x: {$lt: 2}, 'a.b': {$gt: 0, $lt: 9}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b': {$gt: 0}}, node: "
        "{ixscan: {filter: null, pattern: {'x': 1, '$_path': 1, 'a.b': 1},"
        "bounds: {'x': [[-Infinity, 2, true, false]], '$_path': [['a.b','a.b',true,true]], 'a.b': "
        "[[-Infinity,9,true,false]]}}}}}");
    // TODO SERVER-56118 This solution should be generated.
    // assertSolutionExists(
    //     "{fetch: {filter: {'a.b': {$gt: 0}}, node: "
    //     "{ixscan: {filter: null, pattern: {'x': 1, '$_path': 1, 'a.b': 1},"
    //     "bounds: {'x': [[-Infinity, 2, true, false]], '$_path': [['a.b','a.b',true,true]], 'a.b':
    //     "
    //     "[[0,Infinity,false,true]]}}}}}");
}

TEST_F(QueryPlannerWildcardTest,
       CompoundWildcardAllPredsEligibleForIndexUseGenerateCandidatePlans) {
    addWildcardIndex(fromjson("{x: 1, 'a.$**': 1}"), {"a.b", "a.c"});
    runQuery(
        fromjson("{x: {$eq: 2}, 'a.b': {$gt: 0, $lt: 9}, 'a.c': {$gt: 11, $lt: 20}, d: {$gt: 31, "
                 "$lt: 40}}"));

    // TODO SERVER-56118: Should generate 4 plans here. Missing the plans where $gts are bounded
    // instead of $lts.
    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0,$lt: 9},'a.c':{$gt:11},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'x': 1, '$_path': 1, 'a.c': 1},"
        "bounds: {'x': [[2, 2, true, true]], '$_path': [['a.c','a.c',true,true]], 'a.c': "
        "[[-Infinity,20,true,false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {'a.b':{$gt:0},'a.c':{$gt:11,$lt:20},d:{$gt:31,$lt:40}}, node: "
        "{ixscan: {filter: null, pattern: {'x': 1, '$_path': 1, 'a.b': 1},"
        "bounds: {'x': [[2, 2, true, true]], '$_path': [['a.b','a.b',true,true]], 'a.b': "
        "[[-Infinity,9,true,false]]}}}}}");
}
}  // namespace mongo
