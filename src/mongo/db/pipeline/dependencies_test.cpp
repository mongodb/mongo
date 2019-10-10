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
set<string> arrayToSet(const char* (&array)[ArrayLen]) {
    set<string> out;
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

TEST(DependenciesToProjectionTest, ShouldIncludeIdIfNeeded) {
    const char* array[] = {"a", "_id"};
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeEntireIdEvenIfOnlyASubFieldIsNeeded) {
    const char* array[] = {"a", "_id.a"};  // still include whole _id (SERVER-7502)
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 1));
}

TEST(DependenciesToProjectionTest, ShouldNotIncludeSubFieldOfIdIfIdIncluded) {
    const char* array[] = {"a", "_id", "_id.a"};  // handle both _id and subfield
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 1));
}

TEST(DependenciesToProjectionTest, ShouldIncludeFieldPrefixedById) {
    const char* array[] = {"a", "_id", "_id_a"};  // _id prefixed but non-subfield
    DepsTracker deps;
    deps.fields = arrayToSet(array);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(),
                      BSON("_id_a" << 1 << "a" << 1 << "_id" << 1));
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
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireFieldsAndTextScoreIfTextScoreNeededWithoutWholeDocument) {
    const char* array[] = {"a"};  // needTextScore without needWholeDocument
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = arrayToSet(array);
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("a" << 1 << "_id" << 0));
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest, ShouldProduceEmptyObjectIfThereAreNoDependencies) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, false);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
}

TEST(DependenciesToProjectionTest, ShouldReturnEmptyObjectIfOnlyTextScoreIsNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = false;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());

    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(DependenciesToProjectionTest,
     ShouldRequireTextScoreIfNoFieldsPresentButWholeDocumentIsNeeded) {
    DepsTracker deps(DepsTracker::kOnlyTextScore);
    deps.fields = {};
    deps.needWholeDocument = true;
    deps.setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_EQ(deps.metadataDeps().count(), 1u);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

}  // namespace
}  // namespace mongo
