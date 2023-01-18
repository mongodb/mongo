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

#include <set>
#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using std::set;
using std::string;

template <size_t ArrayLen>
OrderedPathSet arrayToSet(const char* (&array)[ArrayLen]) {
    OrderedPathSet out;
    for (size_t i = 0; i < ArrayLen; i++)
        out.insert(array[i]);
    return out;
}

TEST(DependenciesTest, CheckClassConstants) {
    ASSERT_TRUE(DepsTracker::kAllGeoNearData[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_TRUE(DepsTracker::kAllGeoNearData[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_EQ(DepsTracker::kAllGeoNearData.count(), 2);
    ASSERT_TRUE(DepsTracker::kAllMetadata.all());
    ASSERT_EQ(DepsTracker::kOnlyTextScore.count(), 1);
    ASSERT_TRUE(DepsTracker::kOnlyTextScore[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataAvailableAndNeeded) {
    DepsTracker deps(~DepsTracker::kOnlyTextScore);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfAllMetadataAvailableAndNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint, true);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kGeoNearPoint));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataUnavailableAndNotNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, false);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataAvailableAndNotNeeded) {
    DepsTracker deps(~DepsTracker::kOnlyTextScore);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, false);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfMetadataUnavailableButNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true),
                  AssertionException);
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfNoMetadataAvailableButNeeded) {
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true),
                  AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint, true),
                  AssertionException);
}

TEST(DependenciesToProjectionTest, ShouldIncludeAllFieldsAndExcludeIdIfNotSpecified) {
    const char* array[] = {"a", "b"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
}

TEST(DependenciesToProjectionTest, ShouldIncludeFieldEvenIfSuffixOfAnotherIncludedField) {
    const char* array[] = {"a", "ab"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                      BSON("a" << 1 << "ab" << 1 << "_id" << 0));
}

TEST(DependenciesToProjectionTest, ShouldNotIncludeSubFieldIfTopLevelAlreadyIncluded) {
    const char* array[] = {"a", "b", "a.b"};  // a.b included by a
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
}

TEST(DependenciesToProjectionTest, ShouldOnlyIncludeRootLevelPrefixesWithTruncate) {
    const char* array[] = {"a.b", "a.c", "a.c.d", "_id.a"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes),
                      BSON("_id" << 1 << "a" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeDottedPrefixesWithoutTruncate) {
    const char* array[] = {"a.b", "a.c", "a.c.d", "_id.a"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::no),
                      BSON("_id.a" << 1 << "a.b" << 1 << "a.c" << 1));
}

TEST(DependenciesToProjectionTest,
     ShouldIncludeAllRootFieldsAndExcludeIdIfNotSpecifiedWithTruncate) {
    const char* array[] = {"a", "b"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes),
                      BSON("a" << 1 << "b" << 1 << "_id" << 0));
}

TEST(DependenciesToProjectionTest, ShouldIncludeFieldEvenIfSuffixOfAnotherFieldWithTruncate) {
    const char* array[] = {"a", "ab"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes),
                      BSON("a" << 1 << "ab" << 1 << "_id" << 0));
}

TEST(DependenciesToProjectionTest, ExcludeIndirectDescendants) {
    const char* array[] = {"a.b", "_id", "a.b.c.d.e"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("_id" << 1 << "a.b" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeIdIfNeeded) {
    const char* array[] = {"a", "_id"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("_id" << 1 << "a" << 1));
}

TEST(DependenciesToProjectionTest, ShouldNotIncludeEntireIdIfOnlyASubFieldIsNeeded) {
    const char* array[] = {"a", "_id.a"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("_id.a" << 1 << "a" << 1));
}

TEST(DependenciesToProjectionTest, ShouldNotIncludeSubFieldOfIdIfIdIncluded) {
    const char* array[] = {"a", "_id", "_id.a"};  // handle both _id and subfield
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("_id" << 1 << "a" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeFieldPrefixedById) {
    const char* array[] = {"a", "_id", "_id_a"};  // _id prefixed but non-subfield
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                      BSON("_id" << 1 << "_id_a" << 1 << "a" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeFieldPrefixedByIdWhenIdSubfieldIsIncluded) {
    const char* array[] = {"a", "_id.a", "_id_a"};  // _id prefixed but non-subfield
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                      BSON("_id.a" << 1 << "_id_a" << 1 << "a" << 1));
}

// SERVER-66418
TEST(DependenciesToProjectionTest, ChildCoveredByParentWithSpecialChars) {
    // without "_id"
    {
        // This is an important test case because '-' is one of the few chars before '.' in utf-8.
        const char* array[] = {"a", "a-b", "a.b"};
        DepsTracker deps;
        deps.fields = arrayToSet(array);
        ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                          BSON("a" << 1 << "a-b" << 1 << "_id" << 0));
    }
    // with "_id"
    {
        const char* array[] = {"_id", "a", "a-b", "a.b"};
        DepsTracker deps;
        deps.fields = arrayToSet(array);
        ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                          BSON("_id" << 1 << "a" << 1 << "a-b" << 1));
    }
}

TEST(DependenciesToProjectionTest, ShouldOutputEmptyObjectIfEntireDocumentNeeded) {
    const char* array[] = {"a"};  // fields ignored with needWholeDocument
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    deps.needWholeDocument = true;
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
}

TEST(DependenciesToProjectionTest, ShouldOnlyRequestTextScoreIfEntireDocumentAndTextScoreNeeded) {
    const char* array[] = {"a"};  // needTextScore with needWholeDocument
    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    deps.fields = arrayToSet(array);
    deps.needWholeDocument = true;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireFieldsAndTextScoreIfTextScoreNeededWithoutWholeDocument) {
    const char* array[] = {"a"};  // needTextScore without needWholeDocument
    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    deps.fields = arrayToSet(array);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 0));
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest, ShouldProduceEmptyObjectIfThereAreNoDependencies) {
    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, false);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
}

TEST(DependenciesToProjectionTest, ShouldReturnEmptyObjectIfOnlyTextScoreIsNeeded) {
    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());

    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireTextScoreIfNoFieldsPresentButWholeDocumentIsNeeded) {
    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = true;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest, SortFieldPaths) {
    const char* array[] = {"",
                           "A",
                           "_id",
                           "a",
                           "a.b",
                           "a.b.c",
                           "a.c",
                           // '-' char in utf-8 comes before '.' but our special fieldpath sort
                           // puts '.' first so that children directly follow their parents.
                           "a-b",
                           "a-b.ear",
                           "a-bear",
                           "a-bear.",
                           "a🌲",
                           "b",
                           "b.a"
                           "b.aa"
                           "b.🌲d"};
    auto fields = arrayToSet(array);
    // our custom sort will restore the ordering above
    auto itr = fields.begin();
    for (unsigned long i = 0; i < fields.size(); i++) {
        ASSERT_EQ(*itr, array[i]);
        ++itr;
    }
}

TEST(DependenciesToProjectionTest, PathLessThan) {
    auto lessThan = PathComparator();
    ASSERT_FALSE(lessThan("a", "a"));
    ASSERT_TRUE(lessThan("a", "aa"));
    ASSERT_TRUE(lessThan("a", "b"));
    ASSERT_TRUE(lessThan("", "a"));
    ASSERT_TRUE(lessThan("Aa", "aa"));
    ASSERT_TRUE(lessThan("a.b", "ab"));
    ASSERT_TRUE(lessThan("a.b", "a-b"));  // SERVER-66418
    ASSERT_TRUE(lessThan("a.b", "a b"));  // SERVER-66418
    // verify the difference from the standard sort
    ASSERT_TRUE(std::string("a.b") > std::string("a-b"));
    ASSERT_TRUE(std::string("a.b") > std::string("a b"));
    // test unicode behavior
    ASSERT_TRUE(lessThan("a.b", "a🌲"));
    ASSERT_TRUE(lessThan("a.b", "a🌲b"));
    ASSERT_TRUE(lessThan("🌲", "🌳"));  // U+1F332 < U+1F333
    ASSERT_TRUE(lessThan("🌲", "🌲.b"));
    ASSERT_FALSE(lessThan("🌲.b", "🌲"));
}

}  // namespace
}  // namespace mongo
