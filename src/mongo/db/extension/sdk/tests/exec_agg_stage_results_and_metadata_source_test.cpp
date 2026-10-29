// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/unittest/unittest.h"

#include <optional>

namespace mongo::extension::sdk {

namespace {

/**
 * Minimal concrete subclass of ExecAggStageResultsAndMetadataSource used for testing.
 * Implements all required pure virtual methods with no-ops.
 */
class TestMultiStreamSource : public ExecAggStageResultsAndMetadataSource {
public:
    TestMultiStreamSource() : ExecAggStageResultsAndMetadataSource("testMultiStreamSource") {}

    ExtensionGetNextResult getNext(const QueryExecutionContextHandle&,
                                   ::MongoExtensionExecAggStage*) override {
        return ExtensionGetNextResult::eof();
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    BSONObj explain(const QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return BSONObj();
    }
};

using StreamType = ExecAggStageResultsAndMetadataSource::StreamType;

void assertEnvelopedAdvanced(const ExtensionGetNextResult& result,
                             int streamTypeInt,
                             const BSONObj& expectedPayload,
                             std::optional<BSONObj> expectedMetadata = std::nullopt) {
    ASSERT_EQ(result.code, extension::GetNextCode::kAdvanced);
    ASSERT_TRUE(result.resultDocument.has_value());
    ASSERT_EQ(result.resultMetadata.has_value(), expectedMetadata.has_value());

    ASSERT_BSONOBJ_EQ(result.resultDocument->getUnownedBSONObj(),
                      BSON("_streamType" << streamTypeInt << "payload" << expectedPayload));

    if (expectedMetadata) {
        ASSERT_BSONOBJ_EQ(result.resultMetadata->getUnownedBSONObj(), *expectedMetadata);
    }
}

TEST(ExecAggStageResultsAndMetadataSourceTest, Advanced_SingleOverload_DocResult_WrapsEnvelope) {
    TestMultiStreamSource stage;
    assertEnvelopedAdvanced(
        stage.advanced(BSON("x" << 42), StreamType::kDocResult), 0, BSON("x" << 42));
}

TEST(ExecAggStageResultsAndMetadataSourceTest, Advanced_SingleOverload_MetaResult_WrapsEnvelope) {
    TestMultiStreamSource stage;
    assertEnvelopedAdvanced(
        stage.advanced(BSON("x" << 42), StreamType::kMetaResult), 1, BSON("x" << 42));
}

TEST(ExecAggStageResultsAndMetadataSourceTest, Advanced_DualOverload_WrapsEnvelopeAndSetsMetadata) {
    TestMultiStreamSource stage;
    assertEnvelopedAdvanced(
        stage.advanced(BSON("x" << 42), StreamType::kDocResult, BSON("$searchScore" << 0.9)),
        0,
        BSON("x" << 42),
        BSON("$searchScore" << 0.9));
}

TEST(ExecAggStageResultsAndMetadataSourceTest,
     Advanced_DualOverloadWithMetadata_WrapsEnvelopeAndSetsMetadata) {
    TestMultiStreamSource stage;
    assertEnvelopedAdvanced(
        stage.advanced(BSON("x" << 42), StreamType::kMetaResult, BSON("$searchScore" << 0.9)),
        1,
        BSON("x" << 42),
        BSON("$searchScore" << 0.9));
}

}  // namespace
}  // namespace mongo::extension::sdk
