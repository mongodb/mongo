/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class DocumentSourceMockExtension : public DocumentSource {
public:
    static constexpr StringData kStageName = "$mockExtension"_sd;

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

    const char* getSourceName() const override {
        return kStageName.data();
    }

    Id getId() const override {
        return DocumentSource::kUnallocatedId;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const override {
        return Value(Document{{getSourceName(), Value(Document{})}});
    }

protected:
    GetNextResult doGetNext() {
        return GetNextResult::makeEOF();
    }
};

using DocumentSourceExtensionParserTest = AggregationContextFixture;

TEST_F(DocumentSourceExtensionParserTest, ShouldSuccessfullyregisterParser) {
    DocumentSource::registerParser("$customExtension", DocumentSourceMockExtension::createFromBson);

    // Verify registration by parsing a stage with the new parser.
    BSONObj stageSpec = BSON("$customExtension" << BSON("field" << 1));
    auto sourceList = DocumentSource::parse(getExpCtx(), stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);
}

DEATH_TEST_F(DocumentSourceExtensionParserTest, ShouldThrowOnDuplicateRegistration, "28707") {
    DocumentSource::registerParser("$duplicateTest", DocumentSourceMockExtension::createFromBson);

    // Should throw assertion on duplicate registration.
    DocumentSource::registerParser("$duplicateTest", DocumentSourceMockExtension::createFromBson);
}
DEATH_TEST_F(DocumentSourceExtensionParserTest, ShouldThrowOnExistingStage, "28707") {
    // Should throw when trying to override an existing stage that's not eligible for overrides.
    DocumentSource::registerParser("$match", DocumentSourceMockExtension::createFromBson);
}

TEST_F(DocumentSourceExtensionParserTest, ShouldAllowVectorSearchOverride) {
    BSONObj vectorSearchSpec = BSON("$vectorSearch" << BSON("index" << "test"
                                                                    << "path"
                                                                    << "embedding"));
    auto sourceList = DocumentSource::parse(getExpCtx(), vectorSearchSpec);
    ASSERT_TRUE(sourceList.size() > 0);

    // Allow $vectorSearch override without assertion.
    DocumentSource::registerParser("$vectorSearch", DocumentSourceMockExtension::createFromBson);

    // Test that we can override it multiple times.
    DocumentSource::registerParser("$vectorSearch", DocumentSourceMockExtension::createFromBson);

    // Global Initializers are only called once per unit test invocation, so if we change
    // the parserMap for one test, that change will persist. We need to restore the original.
    DocumentSource::registerParser("$vectorSearch", DocumentSourceVectorSearch::createFromBson);
}

TEST_F(DocumentSourceExtensionParserTest, ShouldCreateDocumentSourceFromExtensionParser) {
    DocumentSource::registerParser("$workingExtension",
                                   DocumentSourceMockExtension::createFromBson);

    BSONObj stageSpec = BSON("$workingExtension" << BSON("field" << 1));

    // Test creating DocumentSource using the registered parser.
    auto sourceList = DocumentSource::parse(getExpCtx(), stageSpec);
    ASSERT_EQUALS(sourceList.size(), 1U);
    ASSERT_TRUE(sourceList.front() != nullptr);
    ASSERT_EQUALS(std::string(sourceList.front()->getSourceName()), std::string("$mockExtension"));
}

TEST_F(DocumentSourceExtensionParserTest, ShouldIntegrateWithBuiltinStages) {
    DocumentSource::registerParser("$integrationTest", DocumentSourceMockExtension::createFromBson);

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
