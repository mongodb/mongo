// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

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
    query_shape::SerializationOptions opts =
        query_shape::SerializationOptions::kMarkIdentifiers_FOR_TEST;

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
    query_shape::SerializationOptions opts = {};
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
    unittest::ServerParameterGuard featureFlagController("featureFlagRankFusionFull", true);

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

// SortPatternPart::operator== compares ExpressionMeta by meta type, not pointer identity.
// Two independently-constructed SortPatterns from the same meta spec must compare equal.
TEST(SortPatternEqualityTest, EqualityHoldsForIndependentlyConstructedMetaExpressions) {
    auto expCtx = getExpCtx();
    auto pattern1 = SortPattern(fromjson("{score: {$meta: 'searchScore'}}"), expCtx);
    auto pattern2 = SortPattern(fromjson("{score: {$meta: 'searchScore'}}"), expCtx);
    ASSERT_EQ(pattern1, pattern2);

    auto pattern3 = SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx);
    ASSERT_NE(pattern1, pattern3);
}

// Confirms that explicitly building a SortPattern with ExpressionMeta (as $vectorSearch does)
// produces the same result as parsing the equivalent BSON $meta spec (as an extension might do via
// getSortPattern()). Field name in the BSON key is discarded for $meta sorts.
TEST(SortPatternEqualityTest, ExplicitMetaConstructionEqualsMetaBsonParsed) {
    auto expCtx = getExpCtx();

    SortPattern::SortPatternPart part;
    part.isAscending = false;
    part.expression = make_intrusive<ExpressionMeta>(
        expCtx.get(), DocumentMetadataFields::MetaType::kVectorSearchScore);
    SortPattern explicit_pattern({std::move(part)});

    auto bson_pattern = SortPattern(fromjson("{score: {$meta: 'vectorSearchScore'}}"), expCtx);

    ASSERT_EQ(explicit_pattern, bson_pattern);
}

}  // namespace
}  // namespace mongo
