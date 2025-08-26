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
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

auto getExpCtx() {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

TEST(SerializeSortPatternTest, SerializeAndRedactFieldName) {
    auto expCtx = getExpCtx();
    auto sortPattern = SortPattern(fromjson("{val: 1}"), expCtx);
    SerializationOptions opts = SerializationOptions::kMarkIdentifiers_FOR_TEST;

    // Most basic sort pattern, confirm that field name gets redacted.
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Confirm that multiple sort fields get redacted.
    sortPattern = SortPattern(fromjson("{val: 1, test: -1, third: -1}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1,"HASH<test>":-1,"HASH<third>":-1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Test sort pattern that contains an expression.
    sortPattern = SortPattern(fromjson("{val: 1, test: {$meta: \"randVal\"}}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1,"$computed1":{"$meta":"randVal"}})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Sorting by only an expression results in a made up field name in serialization and therefore
    // doesn't get redacted.
    sortPattern = SortPattern(fromjson("{val: {$meta: \"textScore\"}}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$computed0":{"$meta":"textScore"}})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    sortPattern = SortPattern(fromjson("{'a.b.c': 1}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<a>.HASH<b>.HASH<c>":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));
}

TEST(SerializeSortPatternTest, SerializeNoRedaction) {
    auto expCtx = getExpCtx();
    auto sortPattern = SortPattern(fromjson("{val: 1}"), expCtx);
    SerializationOptions opts = {};
    opts.transformIdentifiers = false;
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"val":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Call serialize() with no options.
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"val":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization));
}


// Throw assertion in the case we have double defined sort order for a field.
TEST(SortStageDefaultTest, WrongSortKeyDefinition) {
    auto expCtx = getExpCtx();
    ASSERT_THROWS_CODE(SortPattern(fromjson("{b: 1, b: 1}"), expCtx), AssertionException, 7472500);

    // Test if the sort order is ignored for the duplication detection.
    ASSERT_THROWS_CODE(SortPattern(fromjson("{b: 1, b: -1}"), expCtx), AssertionException, 7472500);

    // Tests that include subdocuments.
    ASSERT_DOES_NOT_THROW(SortPattern(fromjson("{a:1, 'b.a':1}"), expCtx));

    ASSERT_THROWS_CODE(
        SortPattern(fromjson("{a:1, 'b.a':1, 'b.a':-1}"), expCtx), AssertionException, 7472500);

    // Test the other SortPattern constructor.
    std::vector<SortPattern::SortPatternPart> sortKeys;
    sortKeys.push_back(SortPattern::SortPatternPart{false, FieldPath("a")});
    sortKeys.push_back(SortPattern::SortPatternPart{false, FieldPath("b")});
    sortKeys.push_back(SortPattern::SortPatternPart{true, FieldPath("a")});
    ASSERT_THROWS_CODE(SortPattern(std::move(sortKeys)), AssertionException, 7472501);
}

// Testing expected behavior of 'isSortOnSingleMetaField()' stateless function.
TEST(IsSortOnSingleMetaFieldTest, TestingIsSortOnSingleMetaFieldFn) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);

    auto expCtx = getExpCtx();

    // SortPattern must have a field.
    ASSERT_FALSE(isSortOnSingleMetaField(SortPattern(fromjson("{}"), expCtx)));

    // SortPattern must have one field, but it must be a metadata field.
    ASSERT_FALSE(isSortOnSingleMetaField(SortPattern(fromjson("{a: 1}"), expCtx)));

    // SortPattern cannot have multiple fields.
    ASSERT_FALSE(isSortOnSingleMetaField(SortPattern(fromjson("{a: 1, b: 1}"), expCtx)));

    // SortPattern on a single metadata field, without QueryMetadataBitSet specified should pass for
    // any valid metadata.
    ASSERT_TRUE(isSortOnSingleMetaField(
        SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx)));

    // SortPattern on invalid metadata type should throw.
    ASSERT_THROWS_CODE(isSortOnSingleMetaField(
                           SortPattern(fromjson("{score: {$meta: 'notRealMetadata'}}"), expCtx)),
                       DBException,
                       31138);

    // SortPattern on valid metadata, but with multiple fields should be false.
    ASSERT_FALSE(isSortOnSingleMetaField(
        SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}, a: 1}"), expCtx)));

    // Explicitly specifying the metadata to consider, matching the metadata in the SortPattern
    // should pass.
    ASSERT_TRUE(isSortOnSingleMetaField(
        SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx),
        (1 << DocumentMetadataFields::MetaType::kVectorSearchScore)));

    // Explicitly specifying the metadata to consider, that does not match the metadata in the
    // SortPattern should fail.
    ASSERT_FALSE(
        isSortOnSingleMetaField(SortPattern(fromjson("{score: {$meta: 'searchScore'}}"), expCtx),
                                (1 << DocumentMetadataFields::MetaType::kVectorSearchScore)));

    // Explicitly specifying multiple metadata to consider, one of them matching the meatada in the
    // SortPattern should pass.
    ASSERT_TRUE(isSortOnSingleMetaField(
        SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx),
        ((1 << DocumentMetadataFields::MetaType::kSearchScore) |
         (1 << DocumentMetadataFields::MetaType::kVectorSearchScore))));

    // Explicitly specifying multiple metadata to consider, neither of them matching the meatada in
    // the SortPattern should fail.
    ASSERT_FALSE(isSortOnSingleMetaField(
        SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx),
        ((1 << DocumentMetadataFields::MetaType::kSearchScore) |
         (1 << DocumentMetadataFields::MetaType::kScore))));
}

}  // namespace
}  // namespace mongo
