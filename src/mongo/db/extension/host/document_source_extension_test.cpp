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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

namespace extension::host {

static int fooInitializationCounter = 0;

class LiteParsedDesugarFooTest : public DocumentSourceExtension::LiteParsedDesugar {
public:
    static std::unique_ptr<LiteParsedDesugar> parse(const NamespaceString& nss,
                                                    const BSONElement& spec,
                                                    const LiteParserOptions& options) {
        return std::make_unique<LiteParsedDesugar>(
            spec.fieldName(),
            spec.Obj(),
            Deferred<DesugaredPipelineInitializerType>{[nss, options]() {
                fooInitializationCounter++;
                // Note that these need to be existing server stages or explicitly defined extension
                // stages.
                auto stage1 = BSON("$match" << BSONObj());
                auto stage2 = BSON("$project" << BSONObj());

                std::list<LiteParsedDocSrcPtr> out;
                out.push_back(LiteParsedDocumentSource::parse(nss, stage1, options));
                out.push_back(LiteParsedDocumentSource::parse(nss, stage2, options));
                return out;
            }});
    }
};
}  // namespace extension::host

namespace {
static constexpr std::string_view kNoOpName = "$noOp";

class NoOpLogicalAggStage : public extension::sdk::LogicalAggStage {
public:
    NoOpLogicalAggStage() {}
};

class NoOpAggStageAstNode : public extension::sdk::AggStageAstNode {
public:
    NoOpAggStageAstNode() : extension::sdk::AggStageAstNode(kNoOpName) {}

    std::unique_ptr<extension::sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggStageAstNode> make() {
        return std::make_unique<NoOpAggStageAstNode>();
    }
};

class NoOpAggStageParseNode : public mongo::extension::sdk::AggStageParseNode {
public:
    NoOpAggStageParseNode() : extension::sdk::AggStageParseNode(kNoOpName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new extension::sdk::ExtensionAggStageAstNode(std::make_unique<NoOpAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }
};

class NoOpAggStageDescriptor : public mongo::extension::sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kNoOpName);

    NoOpAggStageDescriptor()
        : mongo::extension::sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {
    }

    std::unique_ptr<mongo::extension::sdk::AggStageParseNode> parse(
        BSONObj stageBson) const override {

        uassert(10596406,
                "Failed to parse $noOpExtension, $noOpExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        uassert(10596407,
                "Failed to parse $noOpExtension, missing boolean field \"foo\"",
                stageDefinition.hasField("foo") && stageDefinition.getField("foo").isBoolean());
        return std::make_unique<NoOpAggStageParseNode>();
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggStageDescriptor> make() {
        return std::make_unique<NoOpAggStageDescriptor>();
    }
};
}  // namespace

auto nss = NamespaceString::createNamespaceString_forTest("document_source_extension_test"_sd);

class DocumentSourceExtensionTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionTest() : DocumentSourceExtensionTest(nss) {}
    explicit DocumentSourceExtensionTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

    /**
     * Helper to create test pipeline.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline) {
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
        LiteParsedDocumentSource::unregisterParser_forTest(NoOpAggStageDescriptor::kStageName);
        mongo::extension::host::DocumentSourceExtension::unregisterParser_forTest(
            NoOpAggStageDescriptor::kStageName);
    }

protected:
    NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_test");

    mongo::extension::sdk::ExtensionAggStageDescriptor _noOpStaticDescriptor{
        NoOpAggStageDescriptor::make()};

    static inline BSONObj kValidSpec =
        BSON(NoOpAggStageDescriptor::kStageName << BSON("foo" << true));
    static inline BSONObj kInvalidSpec = BSON(NoOpAggStageDescriptor::kStageName << BSONObj());
};

TEST_F(DocumentSourceExtensionTest, liteParsedDesugarTest) {
    auto spec = BSON("$foo" << BSONObj());
    auto lp = extension::host::LiteParsedDesugarFooTest::parse(
        nss, spec.firstElement(), LiteParserOptions{});

    // Ensure that we can desugar once.
    const auto& pipeline = lp->getDesugaredPipeline();
    ASSERT_EQUALS(pipeline.size(), 2);
    ASSERT_EQUALS(extension::host::fooInitializationCounter, 1);

    // Ensure that we do not do the work to desugar again the next time we try to access the
    // desugared pipeline.
    const auto& pipelineAgain = lp->getDesugaredPipeline();
    ASSERT_EQ(&pipeline, &pipelineAgain);
    ASSERT_EQUALS(extension::host::fooInitializationCounter, 1);
}

TEST_F(DocumentSourceExtensionTest, parseNoOpSuccess) {
    // Try to parse pipeline with custom extension stage before registering the extension,
    // should fail.
    std::vector<BSONObj> testPipeline{kValidSpec};
    ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 16436);
    // Register the extension stage and try to reparse.
    mongo::extension::host::HostPortal::registerStageDescriptor(
        reinterpret_cast<const ::MongoExtensionAggStageDescriptor*>(&_noOpStaticDescriptor));
    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);

    ASSERT_EQUALS(parsedPipeline->size(), 1u);
    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    ASSERT_EQUALS(std::string(stagePtr->getSourceName()), NoOpAggStageDescriptor::kStageName);
    auto serializedPipeline = parsedPipeline->serializeToBson();
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    ASSERT_BSONOBJ_EQ(serializedPipeline[0], kValidSpec);

    const auto* documentSourceExtension =
        dynamic_cast<const mongo::extension::host::DocumentSourceExtension*>(stagePtr);
    ASSERT_TRUE(documentSourceExtension != nullptr);
    auto extensionStage = exec::agg::buildStage(
        const_cast<mongo::extension::host::DocumentSourceExtension*>(documentSourceExtension));
    // Ensure our stage is indeed a NoOp.
    ASSERT_TRUE(extensionStage->getNext().isEOF());
    Document inputDoc = Document{{"foo", 1}};
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    extensionStage->setSource(mock.get());
    auto next = extensionStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), inputDoc);

    {
        // Test that a parsing failure correctly rethrows the uassert.
        std::vector<BSONObj> testPipeline{kInvalidSpec};
        ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 10596407);
    }
}

}  // namespace mongo
