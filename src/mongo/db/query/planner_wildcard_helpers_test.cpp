/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::wildcard_planning {
namespace {

/**
 * Owns WildcardProjection object for CoreIndexInfo since the latter holds only reference to it.
 */
struct IndexEntryMock {
    IndexEntryMock(BSONObj keyPattern, BSONObj wp, std::set<FieldRef> multiKeyPathSet)
        : emptyMultiKeyPaths{},
          wildcardProjection{WildcardKeyGenerator::createProjectionExecutor(keyPattern, wp)},
          indexEntry{} {
        std::string indexName{"wc_1"};
        const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
        indexEntry = std::make_unique<IndexEntry>(keyPattern,
                                                  type,
                                                  IndexDescriptor::kLatestIndexVersion,
                                                  false,
                                                  emptyMultiKeyPaths,
                                                  multiKeyPathSet,
                                                  true,
                                                  false,
                                                  CoreIndexInfo::Identifier{indexName},
                                                  nullptr,
                                                  BSONObj(),
                                                  nullptr,
                                                  &wildcardProjection);
    }

    MultikeyPaths emptyMultiKeyPaths;
    WildcardProjection wildcardProjection;
    std::unique_ptr<IndexEntry> indexEntry;
};
}  // namespace

TEST(PlannerWildcardHelpersTest, Expand_SingleWildcardIndex_WithProjection) {
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);

    IndexEntryMock wildcardIndex{BSON("$**" << 1), BSON("a" << 1), {FieldRef{"a"_sd}}};

    stdx::unordered_set<std::string> fields{"a", "b"};
    std::vector<IndexEntry> expandedIndexes{};
    expandWildcardIndexEntry(*wildcardIndex.indexEntry, fields, &expandedIndexes);

    ASSERT_EQ(1, expandedIndexes.size());
    ASSERT_BSONOBJ_EQ(expandedIndexes.front().keyPattern, BSON("a" << 1));
    ASSERT_TRUE(expandedIndexes.front().multikey);
    ASSERT_EQ(MultikeyPaths{{0}}, expandedIndexes.front().multikeyPaths);
}

TEST(PlannerWildcardHelpersTest, Expand_SingleWildcardIndex_WithoutProjection) {
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);

    IndexEntryMock wildcardIndex{BSON("$**" << 1), BSONObj{}, {FieldRef{"a"_sd}}};

    stdx::unordered_set<std::string> fields{"a", "b"};
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
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);

    IndexEntryMock wildcardIndex{
        BSON("e.f" << 1 << "$**" << 1 << "m.n" << 1), BSON("a" << 1), {FieldRef{"a"_sd}}};

    stdx::unordered_set<std::string> fields{"a.c", "b"};
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
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);

    IndexEntryMock wildcardIndex{BSON("e.f" << 1 << "b.d" << 1 << "prefix.$**" << 1 << "m.n" << 1),
                                 BSONObj{},
                                 {FieldRef{"prefix.a"_sd}}};

    stdx::unordered_set<std::string> fields{"prefix.a", "prefix.b"};
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

TEST(PlannerWildcardHelpersTest, Expand_CompoundWildcardIndex_NumericComponents) {
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);

    IndexEntryMock wildcardIndex{BSON("e.f" << 1 << "$**" << 1 << "m.n" << 1),
                                 BSON("a.0" << 1 << "b" << 1),
                                 {FieldRef{"a"_sd}}};

    stdx::unordered_set<std::string> fields{"a.0.b", "b"};
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

}  // namespace mongo::wildcard_planning
