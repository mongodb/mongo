/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
