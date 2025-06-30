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

#include "mongo/db/extension/host/document_source_extension.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/extension/host/stage_registry.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

// Start NoOp logical stage.
class NoOpLogicalAggregationStage : public mongo::extension::sdk::LogicalAggregationStage {
public:
    NoOpLogicalAggregationStage() {}
};

// Start NoOp static descriptor.
class NoOpAggregationStageDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$noOpExtension";

    NoOpAggregationStageDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {

        uassert(10596406,
                "Failed to parse $noOpExtension, $noOpExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        uassert(10596407,
                "Failed to parse $noOpExtension, missing boolean field \"foo\"",
                stageDefinition.hasField("foo") && stageDefinition.getField("foo").isBoolean());
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<NoOpAggregationStageDescriptor>();
    }
};

auto nss = NamespaceString::createNamespaceString_forTest("document_source_extension_test"_sd);

class DocumentSourceExtensionTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionTest() : DocumentSourceExtensionTest(nss) {}
    explicit DocumentSourceExtensionTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

    /**
     * Helper to create test pipeline.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> buildTestPipeline(
        const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = getExpCtx();
        expCtx->setNamespaceString(_nss);
        expCtx->setInRouter(false);

        return Pipeline::parse(rawPipeline, expCtx);
    }

    // Runs after each individual test
    void tearDown() override {
        DocumentSourceExtensionTest::unregisterParsers();
    }

    static void unregisterParsers() {
        LiteParsedDocumentSource::unregisterParser_forTest(
            NoOpAggregationStageDescriptor::kStageName);
        mongo::extension::host::DocumentSourceExtension::unregisterParser_forTest(
            NoOpAggregationStageDescriptor::kStageName);
    }

protected:
    NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_test");

    mongo::extension::sdk::ExtensionAggregationStageDescriptor _noOpStaticDescriptor{
        NoOpAggregationStageDescriptor::make()};

    static inline BSONObj kValidSpec =
        BSON(NoOpAggregationStageDescriptor::kStageName << BSON("foo" << true));
    static inline BSONObj kInvalidSpec =
        BSON(NoOpAggregationStageDescriptor::kStageName << BSONObj());
};

namespace {

TEST_F(DocumentSourceExtensionTest, noOpStaticDescriptorTest) {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(&_noOpStaticDescriptor);
    ASSERT_EQUALS(handle.getName(), NoOpAggregationStageDescriptor::kStageName);
    ASSERT_EQUALS(handle.getType(), ::MongoExtensionAggregationStageType::kNoOp);
}

TEST_F(DocumentSourceExtensionTest, parseNoOpSuccess) {
    // Try to parse pipeline with custom extension stage before registering the extension,
    // should fail.
    std::vector<BSONObj> testPipeline{kValidSpec};
    ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 16436);
    // Register the extension stage and try to reparse.
    extension::sdk::enterC([&]() {
        return mongo::extension::host::registerStageDescriptor(
            reinterpret_cast<const ::MongoExtensionAggregationStageDescriptor*>(
                &_noOpStaticDescriptor));
    });
    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);

    ASSERT_EQUALS(parsedPipeline->size(), 1u);
    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    ASSERT_EQUALS(std::string(stagePtr->getSourceName()),
                  NoOpAggregationStageDescriptor::kStageName);
    auto serializedPipeline = parsedPipeline->serializeToBson();
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    ASSERT_BSONOBJ_EQ(serializedPipeline[0], kValidSpec);

    const auto* documentSourceExtension =
        dynamic_cast<const mongo::extension::host::DocumentSourceExtension*>(stagePtr);
    ASSERT_TRUE(documentSourceExtension != nullptr);
    // Ensure our stage is indeed a NoOp.
    ASSERT_TRUE(
        const_cast<mongo::extension::host::DocumentSourceExtension*>(documentSourceExtension)
            ->getNext()
            .isEOF());
    Document inputDoc = Document{{"foo", 1}};
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    const_cast<mongo::extension::host::DocumentSourceExtension*>(documentSourceExtension)
        ->setSource(mock.get());
    auto next =
        const_cast<mongo::extension::host::DocumentSourceExtension*>(documentSourceExtension)
            ->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), inputDoc);

    {
        // Test that a parsing failure correctly rethrows the uassert.
        std::vector<BSONObj> testPipeline{kInvalidSpec};
        ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 10596407);
    }
}
}  // namespace
}  // namespace mongo
