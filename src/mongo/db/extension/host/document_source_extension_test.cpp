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
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/idl/server_parameter_test_controller.h"
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

// Start Desugar logical stage.
class DesugarLogicalAggregationStage : public mongo::extension::sdk::LogicalAggregationStage {
public:
    DesugarLogicalAggregationStage() {}
};

// Start Desugar static descriptor.
class DesugarAsMatchAndLimitDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$matchLimitDesugarExtension";

    DesugarAsMatchAndLimitDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kDesugar) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<DesugarLogicalAggregationStage>();
    }

    BSONArray expand() const override {
        return BSON_ARRAY(BSON("$match" << BSONObj()) << BSON("$limit" << 1));
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<DesugarAsMatchAndLimitDescriptor>();
    }
};

class DesugarToEmptyDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$emptyDesugarExtension";

    DesugarToEmptyDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kDesugar) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<DesugarLogicalAggregationStage>();
    }

    BSONArray expand() const override {
        return {};
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<DesugarToEmptyDescriptor>();
    }
};

class BadDesugarNoExpandOverrideDescriptor
    : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$noExpandOverrideDesugarExtension";

    BadDesugarNoExpandOverrideDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kDesugar) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<DesugarLogicalAggregationStage>();
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<BadDesugarNoExpandOverrideDescriptor>();
    }
};

class BadDesugarArrayElementsDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$badArrayEltsDesugarExtension";

    BadDesugarArrayElementsDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kDesugar) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<DesugarLogicalAggregationStage>();
    }

    BSONArray expand() const override {
        return BSON_ARRAY(1);
    }

    static inline std::unique_ptr<mongo::extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<BadDesugarArrayElementsDescriptor>();
    }
};

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

    mongo::extension::sdk::ExtensionAggregationStageDescriptor _matchLimitDesugarStaticDescriptor{
        DesugarAsMatchAndLimitDescriptor::make()};

    mongo::extension::sdk::ExtensionAggregationStageDescriptor _emptyDesugarStaticDescriptor{
        DesugarToEmptyDescriptor::make()};

    mongo::extension::sdk::ExtensionAggregationStageDescriptor
        _badNoExpandOverrideDesugarStaticDescriptor{BadDesugarNoExpandOverrideDescriptor::make()};

    mongo::extension::sdk::ExtensionAggregationStageDescriptor _badArrayEltsDesugarStaticDescriptor{
        BadDesugarArrayElementsDescriptor::make()};

    static inline BSONObj kValidSpec =
        BSON(NoOpAggregationStageDescriptor::kStageName << BSON("foo" << true));
    static inline BSONObj kInvalidSpec =
        BSON(NoOpAggregationStageDescriptor::kStageName << BSONObj());
};

namespace {

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
    mongo::extension::host::registerStageDescriptor(
        reinterpret_cast<const ::MongoExtensionAggregationStageDescriptor*>(
            &_noOpStaticDescriptor));
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

TEST_F(DocumentSourceExtensionTest, DesugarStaticDescriptorTest) {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(
        &_matchLimitDesugarStaticDescriptor);
    ASSERT_EQUALS(handle.getName(), DesugarAsMatchAndLimitDescriptor::kStageName);
    ASSERT_EQUALS(handle.getType(), ::MongoExtensionAggregationStageType::kDesugar);
}

TEST_F(DocumentSourceExtensionTest, MatchLimitDesugarExpansionSucceedsTest) {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(
        &_matchLimitDesugarStaticDescriptor);

    auto vec = handle.getExpandedPipelineVec();
    ASSERT_EQ(vec.size(), 2U);
    ASSERT_TRUE(vec[0].hasField("$match"));
    ASSERT_TRUE(vec[1].hasField("$limit"));
    ASSERT_EQ(vec[1].getIntField("$limit"), 1) << vec[1].toString();
}

TEST_F(DocumentSourceExtensionTest, EmptyDesugarExpansionSucceedsTest) {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(
        &_emptyDesugarStaticDescriptor);

    auto vec = handle.getExpandedPipelineVec();
    ASSERT_EQ(vec.size(), 0U);
}

DEATH_TEST_F(DocumentSourceExtensionTest, NoExpandOverrideDesugarExpansionFails, "11038200") {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(
        &_badNoExpandOverrideDesugarStaticDescriptor);
    [[maybe_unused]] auto result = handle.getExpandedPipelineVec();
}

TEST_F(DocumentSourceExtensionTest, BadArrayElementsDesugarExpansionFails) {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(
        &_badArrayEltsDesugarStaticDescriptor);
    ASSERT_THROWS_CODE(handle.getExpandedPipelineVec(), DBException, ErrorCodes::TypeMismatch);
}

DEATH_TEST_F(DocumentSourceExtensionTest, NonDesugarExtensionExpansionFails, "11038201") {
    extension::host::ExtensionAggregationStageDescriptorHandle handle(&_noOpStaticDescriptor);
    [[maybe_unused]] auto result = handle.getExpandedPipelineVec();
}
}  // namespace
}  // namespace mongo
