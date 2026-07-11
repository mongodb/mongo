// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "ext/alloc_traits.h"

#include <set>

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"

// IWYU pragma: no_include "boost/intrusive/detail/algorithm.hpp"
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/wildcard_test_utils.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>


namespace mongo::wildcard_planning {
using namespace std::literals::string_view_literals;

TEST(PlannerWildcardHelpersTest, Expand_SingleWildcardIndex_WithProjection) {
    WildcardIndexEntryMock wildcardIndex{BSON("$**" << 1), BSON("a" << 1), {FieldRef{"a"sv}}};

    std::set<std::string> fields{"a", "b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(1, expandedIndexes.size());
    ASSERT_BSONOBJ_EQ(expandedIndexes.front().keyPattern, BSON("a" << 1));
    ASSERT_TRUE(expandedIndexes.front().multikey);
    ASSERT_EQ(MultikeyPaths{{0}}, expandedIndexes.front().multikeyPaths);
}

TEST(PlannerWildcardHelpersTest, Expand_SingleWildcardIndex_WithoutProjection) {
    WildcardIndexEntryMock wildcardIndex{BSON("$**" << 1), BSONObj{}, {FieldRef{"a"sv}}};

    std::set<std::string> fields{"a", "b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(2, expandedIndexes.size());

    const auto keyPatternA = BSON("a" << 1);
    const auto keyPatternB = BSON("b" << 1);
    for (const auto& index : expandedIndexes) {
        if (index.keyPattern.woCompare(keyPatternA) == 0) {
            ASSERT_TRUE(index.multikey);
            ASSERT_EQ(MultikeyPaths{{0}}, index.multikeyPaths);
        } else if (index.keyPattern.woCompare(keyPatternB) == 0) {
            ASSERT_FALSE(index.multikey);
            ASSERT_EQ(MultikeyPaths{{}}, index.multikeyPaths);
        } else {
            FAIL("unexpected index entry");
        }
    }
}

TEST(PlannerWildcardHelpersTest, Expand_CompoundWildcardIndex_WithProjection) {
    WildcardIndexEntryMock wildcardIndex{
        BSON("e.f" << 1 << "$**" << 1 << "m.n" << 1), BSON("a" << 1), {FieldRef{"a"sv}}};

    std::set<std::string> fields{"a.c", "b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    MultikeyPaths expectedMks{{}, {0}, {}};
    ASSERT_EQ(1, expandedIndexes.size());
    ASSERT_BSONOBJ_EQ(expandedIndexes.front().keyPattern,
                      BSON("e.f" << 1 << "a.c" << 1 << "m.n" << 1));
    ASSERT_TRUE(expandedIndexes.front().multikey);
    ASSERT_EQ(1, expandedIndexes.front().wildcardFieldPos);
    ASSERT_EQ(expectedMks, expandedIndexes.front().multikeyPaths);
}

TEST(PlannerWildcardHelpersTest, Expand_CompoundWildcardIndex_WithoutProjection) {
    WildcardIndexEntryMock wildcardIndex{
        BSON("e.f" << 1 << "b.d" << 1 << "prefix.$**" << 1 << "m.n" << 1),
        BSONObj{},
        {FieldRef{"prefix.a"sv}}};

    std::set<std::string> fields{"prefix.a", "prefix.b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(2, expandedIndexes.size());

    const auto keyPatternA = BSON("e.f" << 1 << "b.d" << 1 << "prefix.a" << 1 << "m.n" << 1);
    const auto keyPatternB = BSON("e.f" << 1 << "b.d" << 1 << "prefix.b" << 1 << "m.n" << 1);

    MultikeyPaths expectedMksA{{}, {}, {1}, {}};
    MultikeyPaths expectedMksB{{}, {}, {}, {}};

    for (const auto& index : expandedIndexes) {
        if (index.keyPattern.woCompare(keyPatternA) == 0) {
            ASSERT_TRUE(index.multikey);
            ASSERT_EQ(expectedMksA, index.multikeyPaths);
        } else if (index.keyPattern.woCompare(keyPatternB) == 0) {
            ASSERT_FALSE(index.multikey);
            ASSERT_EQ(expectedMksB, index.multikeyPaths);
        } else {
            FAIL("unexpected index entry");
        }
        ASSERT_EQ(2, index.wildcardFieldPos);
    }
}

TEST(PlannerWildcardHelpersTest, FinalizeBasicPatternInCompoundWildcardIndexScanConfiguration) {
    WildcardIndexEntryMock wildcardIndex{
        BSON("a" << 1 << "$**" << 1 << "c" << 1), BSON("b" << 1), {FieldRef{"b"sv}}};
    std::vector<IndexEntry> expandedIndexes{};
    std::set<std::string> fields{"b"};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(1, expandedIndexes.size());

    const auto& expandedIndex = expandedIndexes[0];

    MultikeyPaths expectedMks{{}, {0}, {}};
    ASSERT_EQ(expectedMks, expandedIndex.multikeyPaths);

    // Create an 'IndexScanNode' with the expanded 'IndexEntry' for testing finalization.
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode idxScan{testNss, expandedIndex};

    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    ietBuilders.resize(3);
    idxScan.bounds.fields = {{"a"}, {"b"}, {"c"}};
    finalizeWildcardIndexScanConfiguration(&idxScan, &ietBuilders);

    auto expectedPattern = BSON("a" << 1 << "$_path" << 1 << "b" << 1 << "c" << 1);
    ASSERT_EQ(expectedPattern.woCompare(idxScan.index.keyPattern), 0);
    ASSERT_EQ(4, ietBuilders.size());
    ASSERT_EQ(4, idxScan.bounds.fields.size());
    ASSERT_EQ("$_path", idxScan.bounds.fields[idxScan.index.wildcardFieldPos - 1].name);
    ASSERT_EQ(2, idxScan.index.wildcardFieldPos);

    MultikeyPaths expectedExpandedMks{{}, {}, {0}, {}};
    ASSERT_EQ(expectedExpandedMks, idxScan.index.multikeyPaths);
}

TEST(PlannerWildcardHelpersTest, AddSubpathBoundsIfBoundsOverlapWithObjects) {
    WildcardIndexEntryMock wildcardIndex{BSON("$**" << 1 << "b" << 1), BSON("a" << 1), {}};
    std::vector<IndexEntry> expandedIndexes{};
    std::set<std::string> fields{"a"};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(1, expandedIndexes.size());

    const auto& expandedIndex = expandedIndexes[0];

    // Create an 'IndexScanNode' with the expanded 'IndexEntry' for testing finalization.
    auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    IndexScanNode idxScan{testNss, expandedIndex};

    std::vector<interval_evaluation_tree::Builder> ietBuilders;
    ietBuilders.resize(2);
    idxScan.bounds.fields = {{"a"}, {"b"}};
    auto objectPointInterval = fromjson("{'': {a: 1}, '': {a: 1}}");
    idxScan.bounds.fields[0].intervals.push_back({objectPointInterval, true, true});
    finalizeWildcardIndexScanConfiguration(&idxScan, &ietBuilders);

    // Because the interval "[{a: 1}, {a: 1}]" overlaps with the Object type bracket we should add
    // all sub-paths to $_path's interval by adding a range interval - ["a.", "a/")
    ASSERT_EQ(2, idxScan.bounds.fields[0].intervals.size());
    ASSERT_EQ("[\"a\", \"a\"]", idxScan.bounds.fields[0].intervals[0].toString(false));
    ASSERT_EQ("[\"a.\", \"a/\")", idxScan.bounds.fields[0].intervals[1].toString(false));
}

TEST(PlannerWildcardHelpersTest, GetCorrectWildcardElement) {
    WildcardIndexEntryMock wildcardIndex{BSON("$**" << 1 << "b" << 1), BSON("a" << 1), {}};
    wildcardIndex.indexEntry->wildcardFieldPos = 0;
    auto elem = wildcardIndex.indexEntry->getWildcardField();
    ASSERT_EQ(0, elem.woCompare(BSON("$**" << 1).firstElement()));

    WildcardIndexEntryMock wildcardIndex2{
        BSON("a" << 1 << "$**" << 1 << "b" << 1), BSON("c" << 1), {}};
    wildcardIndex2.indexEntry->wildcardFieldPos = 1;
    elem = wildcardIndex2.indexEntry->getWildcardField();
    ASSERT_EQ(0, elem.woCompare(BSON("$**" << 1).firstElement()));
}

TEST(PlannerWildcardHelpersTest, Expand_CompoundWildcardIndex_NumericComponents) {
    WildcardIndexEntryMock wildcardIndex{BSON("e.f" << 1 << "$**" << 1 << "m.n" << 1),
                                         BSON("a.0" << 1 << "b" << 1),
                                         {FieldRef{"a"sv}}};

    std::set<std::string> fields{"a.0.b", "b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    // Expanded IndexEntry {e.f: 1, a.0.b: 1, m.n: 1} is exclauded since this is the case when the
    // query path lies along a $** projection through an array index. See comment to
    // 'validateNumericPathComponents' in planner_wildcard_helper.cpp function for details.
    MultikeyPaths expectedMks{{}, {}, {}};
    ASSERT_EQ(1, expandedIndexes.size());
    ASSERT_BSONOBJ_EQ(expandedIndexes.front().keyPattern,
                      BSON("e.f" << 1 << "b" << 1 << "m.n" << 1));
    ASSERT_FALSE(expandedIndexes.front().multikey);
    ASSERT_EQ(expectedMks, expandedIndexes.front().multikeyPaths);
}

DEATH_TEST(PlannerWildcardHelpersTestDeathTest, InvalidIndexExpansion, "11390001") {
    WildcardIndexEntryMock wildcardIndex{BSON("a" << 1 << "$**" << 1), BSON("_id" << 0), {}};
    std::set<std::string> fields{"a"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);
}

DEATH_TEST(PlannerWildcardHelpersTestDeathTest, AnotherInvalidIndexExpansion, "11390001") {
    WildcardIndexEntryMock wildcardIndex{BSON("$**" << 1 << "a" << 1), BSON("_id" << 0), {}};
    std::set<std::string> fields{"a"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);
}

}  // namespace mongo::wildcard_planning
