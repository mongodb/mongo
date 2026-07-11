// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(MockExtension)

class DocumentSourceMockExtension : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$mockExtension"sv;

    DocumentSourceMockExtension(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    static boost::intrusive_ptr<DocumentSourceMockExtension> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceMockExtension(expCtx);
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return create(expCtx);
    }

    StageConstraints constraints(
        PipelineSplitState pipeState = PipelineSplitState::kUnsplit) const override {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed);
    }

    std::string_view getSourceName() const override {
        return kStageName;
    }

    Id getId() const override {
        return DocumentSource::kUnallocatedId;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        return boost::none;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const override {
        return Value(Document{{getSourceName(), Value(Document{})}});
    }

protected:
    GetNextResult doGetNext() {
        return GetNextResult::makeEOF();
    }
};

// Register the mapping from MockExtensionStageParams to DocumentSourceMockExtension. This allows
// DocumentSource::parse() to create DocumentSourceMockExtension instances when parsing stages that
// use MockExtensionLiteParsed.
REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(mockExtension,
                                                   DocumentSourceMockExtension,
                                                   MockExtensionStageParams);

using DocumentSourceExtensionParserTest = AggregationContextFixture;

TEST_F(DocumentSourceExtensionParserTest, ShouldSuccessfullyregisterParser) {
    LiteParsedDocumentSource::registerParser(
        "$customExtension",
        {.parser = MockExtensionLiteParsed::parse,
         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
         .allowedWithClientType = AllowedWithClientType::kAny});

    // Verify registration by parsing a stage with the new parser.
    BSONObj stageSpec = BSON("$customExtension" << BSON("field" << 1));
    auto sourceList = DocumentSource::parse(getExpCtx(), stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);
}

using DocumentSourceExtensionParserTestDeathTest = DocumentSourceExtensionParserTest;

TEST_F(DocumentSourceExtensionParserTest, ShouldCreateDocumentSourceFromExtensionParser) {
    LiteParsedDocumentSource::registerParser(
        "$workingExtension",
        {.parser = MockExtensionLiteParsed::parse,
         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
         .allowedWithClientType = AllowedWithClientType::kAny});

    BSONObj stageSpec = BSON("$workingExtension" << BSON("field" << 1));

    // Test creating DocumentSource using the registered parser.
    auto sourceList = DocumentSource::parse(getExpCtx(), stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);
    ASSERT_TRUE(sourceList.front() != nullptr);
    ASSERT_EQUALS(std::string(sourceList.front()->getSourceName()), std::string("$mockExtension"));
}

TEST_F(DocumentSourceExtensionParserTest, ShouldIntegrateWithBuiltinStages) {
    LiteParsedDocumentSource::registerParser(
        "$integrationTest",
        {.parser = MockExtensionLiteParsed::parse,
         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
         .allowedWithClientType = AllowedWithClientType::kAny});

    // Check for both built-in and extension parsers using a vector to avoid duplication.
    std::vector<std::pair<std::string, BSONObj>> stageTests = {
        {"$match", BSON("$match" << BSON("field" << "value"))},
        {"$group", BSON("$group" << BSON("_id" << "$field"))},
        {"$limit", BSON("$limit" << 10)},
        {"$integrationTest", BSON("$integrationTest" << BSON("field" << 1))}};

    for (const auto& [stageName, stageSpec] : stageTests) {
        auto sourceList = DocumentSource::parse(getExpCtx(), stageSpec);
        ASSERT_TRUE(sourceList.size() > 0) << "Failed to parse stage: " << stageName;
    }
}
}  // namespace
}  // namespace mongo
