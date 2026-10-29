// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <bitset>
#include <compare>
#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
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
    ASSERT_EQ(DepsTracker::kOnlyTextScore.count(), 2);
    ASSERT_TRUE(DepsTracker::kOnlyTextScore[DocumentMetadataFields::kTextScore]);
    ASSERT_TRUE(DepsTracker::kOnlyTextScore[DocumentMetadataFields::kScore]);
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataAvailableAndNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfTextMetadataUnavailableButNeeded) {
    DepsTracker deps(DepsTracker::kAllGeoNearData);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore), AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kScore), AssertionException);
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfGeoMetadataUnavailableButNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearDist), AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint), AssertionException);
}


TEST(DependenciesNeedsMetadataTest, ShouldThrowIfScoreDetailsUnavailableButNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kScoreDetails), AssertionException);
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfNotTrackingAvailableMetadataAndIsNeeded) {
    DepsTracker deps((DepsTracker::NoMetadataValidation()));
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

// Same as above but tests that the default constructor sets availableMetadata =
// NoMetadataValidation.
TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfDefaultNotTrackingAvailableMetadataAndIsNeeded) {
    DepsTracker deps;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfAllMetadataAvailableAndNeeded) {
    DepsTracker deps(DepsTracker::kAllMetadata);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kGeoNearPoint));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

// Tests that any field chosen to be validated should throw if unavailable when requested, and that
// no other meta fields throw in that case.
TEST(DependenciesNeedsMetadataTest, OnlyChosenMetadataFieldsShouldThrowIfUnavailable) {
    static const std::set<DocumentMetadataFields::MetaType> kMetadataFieldsToBeValidated = {
        DocumentMetadataFields::MetaType::kTextScore,
        DocumentMetadataFields::MetaType::kGeoNearDist,
        DocumentMetadataFields::MetaType::kGeoNearPoint,
        DocumentMetadataFields::MetaType::kScore,
        DocumentMetadataFields::MetaType::kSearchScore,
        DocumentMetadataFields::MetaType::kVectorSearchScore,
        DocumentMetadataFields::MetaType::kScoreDetails,
        DocumentMetadataFields::MetaType::kSearchRootDocumentId,
    };

    DepsTracker deps(DepsTracker::kNoMetadata);

    for (int i = 1; i < DocumentMetadataFields::kNumFields; i++) {
        DocumentMetadataFields::MetaType type = static_cast<DocumentMetadataFields::MetaType>(i);
        if (kMetadataFieldsToBeValidated.contains(type)) {
            ASSERT_THROWS(deps.setNeedsMetadata(type), AssertionException);
        } else {
            ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(type));
        }
    }
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedScorePopulatedByTextScore) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.setNeedsMetadata(DocumentMetadataFields::kScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kScore));
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedScoreDetailsPopulatedBySearchScoreDetails) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.setMetadataAvailable(DocumentMetadataFields::kSearchScoreDetails);
    deps.setNeedsMetadata(DocumentMetadataFields::kScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kScore));
}

// Only a handful of metadata fields will throw if you try to reference them when not available.
// This confirms that trying to access other fields won't throw even if the field isn't marked
// available.
TEST(DependenciesNeedsMetadataTest, ShouldAlwaysSucceedForNonValidatedField) {
    DepsTracker deps(DepsTracker::kNoMetadata);

    deps.setNeedsMetadata(DocumentMetadataFields::kRandVal);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kRandVal));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kSearchSequenceToken);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kSearchSequenceToken));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataUnavailableAndNotNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataAvailableAndNotNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfNotTrackingAvailableMetadataAndIsNotNeeded) {
    DepsTracker deps((DepsTracker::NoMetadataValidation()));
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_FALSE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfSetTextMetadataAvailableThenIsNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    // Set text score metadata available, even though no metadata was originally available.
    deps.setMetadataAvailable(DocumentMetadataFields::kTextScore);

    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kScore);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kScore));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfSetGeoMetadataAvailableThenIsNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    // Set text score metadata available, even though no metadata was originally available.
    deps.setMetadataAvailable(DepsTracker::kAllGeoNearData);

    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearDist);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kGeoNearDist));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kGeoNearPoint));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfSetScoreDetailsMetadataAvailableThenIsNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    // Set search score details metadata available, even though no metadata was originally
    // available.
    deps.setMetadataAvailable(DocumentMetadataFields::kSearchScoreDetails);

    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kSearchScoreDetails);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kSearchScoreDetails));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());

    deps.setNeedsMetadata(DocumentMetadataFields::kScoreDetails);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kScoreDetails));
    ASSERT_TRUE(deps.getNeedsAnyMetadata());
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfSetTextMetadataAvailableButGeoIsNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    // Set text score metadata available, even though no metadata was originally available.
    deps.setMetadataAvailable(DocumentMetadataFields::kTextScore);

    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearDist), AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint), AssertionException);
}

TEST(DependenciesNeedsMetadataTest, ShouldThrowIfSetGeoMetadataAvailableButTextScoreIsNeeded) {
    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    // Set geo metadata available, even though no metadata was originally available.
    deps.setMetadataAvailable(DepsTracker::kAllGeoNearData);

    ASSERT_FALSE(deps.getNeedsAnyMetadata());

    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kScore), AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore), AssertionException);
}

// TODO SERVER-100443 This should apply to all validated fields, not just "score" and
// "scoreDetails".
TEST(DependenciesNeedsMetadataTest, OnlyScoreAndScoreDetailsShouldThrowIfAvailableMetadataCleared) {
    DepsTracker deps(DepsTracker::kAllMetadata);
    deps.clearMetadataAvailable();

    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kScore), AssertionException);
    ASSERT_THROWS(deps.setNeedsMetadata(DocumentMetadataFields::kScoreDetails), AssertionException);

    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kSearchScore));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kSearchScoreDetails));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearDist));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint));
}

TEST(DependenciesNeedsMetadataTest, ShouldSucceedIfMetadataClearedAndRepopulated) {
    DepsTracker deps(DepsTracker::kAllMetadata);
    deps.clearMetadataAvailable();
    deps.setMetadataAvailable(DocumentMetadataFields::kScore);
    deps.setMetadataAvailable(DocumentMetadataFields::kScoreDetails);

    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kScore));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kScoreDetails));

    // TODO SERVER-100443 These fields would need to be set available to pass validation.
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kSearchScore));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kSearchScoreDetails));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearDist));
    ASSERT_DOES_NOT_THROW(deps.setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint));
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
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = arrayToSet(array);
    deps.needWholeDocument = true;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireFieldsAndTextScoreIfTextScoreNeededWithoutWholeDocument) {
    const char* array[] = {"a"};  // needTextScore without needWholeDocument
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = arrayToSet(array);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 0));
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest, ShouldProduceEmptyObjectIfThereAreNoDependencies) {
    DepsTracker deps;
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
}

TEST(DependenciesToProjectionTest, ShouldReturnEmptyObjectIfOnlyTextScoreIsNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());

    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireTextScoreIfNoFieldsPresentButWholeDocumentIsNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = true;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore);
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
                           "b.a",
                           "b.aa",
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

    // Test std::string type comparison.
    ASSERT_FALSE(lessThan(std::string("a"), std::string("a")));
    ASSERT_TRUE(lessThan(std::string("a"), std::string("aa")));
    ASSERT_TRUE(lessThan(std::string("a"), std::string("b")));
    ASSERT_TRUE(lessThan(std::string(""), std::string("a")));
    ASSERT_TRUE(lessThan(std::string("Aa"), std::string("aa")));
    ASSERT_TRUE(lessThan(std::string("a.b"), std::string("ab")));
    ASSERT_TRUE(lessThan(std::string("a.b"), std::string("a-b")));  // SERVER-66418
    ASSERT_TRUE(lessThan(std::string("a.b"), std::string("a b")));  // SERVER-66418
    // Verify the difference from the standard sort.
    ASSERT_TRUE(std::string("a.b") > std::string("a-b"));
    ASSERT_TRUE(std::string("a.b") > std::string("a b"));
    // Test unicode behavior.
    ASSERT_TRUE(lessThan(std::string("a.b"), std::string("a🌲")));
    ASSERT_TRUE(lessThan(std::string("a.b"), std::string("a🌲b")));
    ASSERT_TRUE(lessThan(std::string("🌲"), std::string("🌳")));  // U+1F332 < U+1F333
    ASSERT_TRUE(lessThan(std::string("🌲"), std::string("🌲.b")));
    ASSERT_FALSE(lessThan(std::string("🌲.b"), std::string("🌲")));

    // Test std::string_view type comparison.
    ASSERT_FALSE(lessThan(std::string_view("a"), std::string_view("a")));
    ASSERT_TRUE(lessThan(std::string_view("a"), std::string_view("aa")));
    ASSERT_TRUE(lessThan(std::string_view("a"), std::string_view("b")));
    ASSERT_TRUE(lessThan(std::string_view(""), std::string_view("a")));
    ASSERT_TRUE(lessThan(std::string_view("Aa"), std::string_view("aa")));
    ASSERT_TRUE(lessThan(std::string_view("a.b"), std::string_view("ab")));

    ASSERT_TRUE(lessThan(std::string_view("a.b"), std::string_view("a-b")));  // SERVER-66418
    ASSERT_TRUE(lessThan(std::string_view("a.b"), std::string_view("a b")));  // SERVER-66418

    // Verify the difference from the standard sort.
    ASSERT_TRUE(std::string_view("a.b") > std::string_view("a-b"));
    ASSERT_TRUE(std::string_view("a.b") > std::string_view("a b"));

    // Test unicode behavior.
    ASSERT_TRUE(lessThan(std::string_view("a.b"), std::string_view("a🌲")));
    ASSERT_TRUE(lessThan(std::string_view("a.b"), std::string_view("a🌲b")));
    ASSERT_TRUE(lessThan(std::string_view("🌲"), std::string_view("🌳")));  // U+1F332 < U+1F333
    ASSERT_TRUE(lessThan(std::string_view("🌲"), std::string_view("🌲.b")));
    ASSERT_FALSE(lessThan(std::string_view("🌲.b"), std::string_view("🌲")));
}

}  // namespace
}  // namespace mongo
