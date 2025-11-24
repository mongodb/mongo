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

#include "mongo/db/pipeline/desugarer.h"

#include "mongo/db/extension/host/document_source_extension_expandable.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DesugarerTest : public AggregationContextFixture {
public:
    DesugarerTest() : DesugarerTest(_nss) {}
    explicit DesugarerTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {
        Desugarer::registerStageExpander(
            extension::host::DocumentSourceExtensionExpandable::id,
            extension::host::DocumentSourceExtensionExpandable::stageExpander);
    }

    void setUp() override {
        AggregationContextFixture::setUp();
        extension::sdk::HostServicesHandle::setHostServices(
            extension::host_connector::HostServicesAdapter::get());
    }

protected:
    static inline NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "desugarer_test");

    // Test stage descriptors from the SDK test helpers.
    extension::sdk::ExtensionAggStageDescriptor _expandToExtAstDescriptor{
        extension::sdk::shared_test_stages::ExpandToExtAstDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptor _expandToExtParseDescriptor{
        extension::sdk::shared_test_stages::ExpandToExtParseDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptor _topDescriptor{
        extension::sdk::shared_test_stages::TopDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptor _expandToHostParseDescriptor{
        extension::sdk::shared_test_stages::ExpandToHostParseDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptor _expandToMixedDescriptor{
        extension::sdk::shared_test_stages::ExpandToMixedDescriptor::make()};
};

TEST_F(DesugarerTest, NoopOnNonExpandableStages) {
    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::create(getExpCtx()));
    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2UL);

    Desugarer(pipeline.get())();

    ASSERT_EQ(pipeline->size(), 2UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsSingleExpandableToHostStageInPlace) {
    // Create [$mock, $expandToHostParse, $mock].
    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto rawStage =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto expandable = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawStage, extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    sources.push_back(expandable);

    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 3UL);

    Desugarer(pipeline.get())();

    // Expect: [$mock, $match, $mock].
    ASSERT_EQ(pipeline->size(), 3UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsHeadExpandableStage) {
    // Create [$expandToHostParse, $mock].
    DocumentSourceContainer sources;

    auto rawHost =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto headExp = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawHost, extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    sources.push_back(headExp);

    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2UL);

    Desugarer(pipeline.get())();

    // Expect [$match, $mock].
    ASSERT_EQ(pipeline->size(), 2UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsTailExpandableStage) {
    // Create [$mock, $expandToHostParse].
    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto rawHost =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto tailExp = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawHost, extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    sources.push_back(tailExp);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2UL);

    Desugarer(pipeline.get())();

    // Expect [$mock, $match].
    ASSERT_EQ(pipeline->size(), 2UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsSingleExpandToExtAstOnly) {
    // Create [$expandToExtAst].
    DocumentSourceContainer sources;

    auto raw =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToExtAstName) << BSONObj());
    auto stage = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), raw, extension::AggStageDescriptorHandle(&_expandToExtAstDescriptor));
    sources.push_back(stage);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 1UL);

    Desugarer(pipeline.get())();

    // Expect: [extOptimizable].
    ASSERT_EQ(pipeline->size(), 1UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
}

TEST_F(DesugarerTest, ExpandsSingleExpandToExtParseOnly) {
    // Create [$expandToExtParse].
    DocumentSourceContainer sources;

    auto raw =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToExtParseName) << BSONObj());
    auto stage = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), raw, extension::AggStageDescriptorHandle(&_expandToExtParseDescriptor));
    sources.push_back(stage);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 1UL);

    Desugarer(pipeline.get())();

    // Expect: [extOptimizable].
    ASSERT_EQ(pipeline->size(), 1UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
}

TEST_F(DesugarerTest, ExpandsSingleExpandToHostParseOnly) {
    // Create [$expandToHostParse].
    DocumentSourceContainer sources;

    auto raw =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto stage = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), raw, extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    sources.push_back(stage);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 1UL);

    Desugarer(pipeline.get())();

    // Expect: [$match].
    ASSERT_EQ(pipeline->size(), 1UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsRecursiveTopToLeaves) {
    // Create [$top]. Top -> [MidA, MidB] -> [LeafA, LeafB, LeafC, LeafD].
    DocumentSourceContainer sources;

    auto raw = BSON(std::string(extension::sdk::shared_test_stages::kTopName) << BSONObj());
    auto stage = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), raw, extension::AggStageDescriptorHandle(&_topDescriptor));
    sources.push_back(stage);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 1UL);

    Desugarer(pipeline.get())();

    // Expect: [extOptimizable($leafA), extOptimizable($leafB), extOptimizable($leafC),
    //          extOptimizable($leafD)].
    ASSERT_EQ(pipeline->size(), 4UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              std::string(extension::sdk::shared_test_stages::kLeafAName));
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              std::string(extension::sdk::shared_test_stages::kLeafBName));
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              std::string(extension::sdk::shared_test_stages::kLeafCName));
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              std::string(extension::sdk::shared_test_stages::kLeafDName));
}

TEST_F(DesugarerTest, ExpandsMixedToMultipleStagesSplicingIntoPipeline) {
    // Create [$mock, $expandToMixed, $mock]. expandToMixed -> [extNoOp, extNoOp,
    // $match].
    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto rawStage =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToMixedName) << BSONObj());
    auto expandable = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawStage, extension::AggStageDescriptorHandle(&_expandToMixedDescriptor));
    sources.push_back(expandable);

    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 3UL);

    Desugarer(pipeline.get())();

    // Expect [$mock, extNoOp, extNoOp, $match, $idLookup, $mock].
    ASSERT_EQ(pipeline->size(), 6UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
}

TEST_F(DesugarerTest, ExpandsMultipleExpandablesSequentially) {
    // Create [$expandToHostParse, $expandToMixed].
    DocumentSourceContainer sources;

    auto rawHost =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToHostParseName) << BSONObj());
    auto hostExp = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawHost, extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    sources.push_back(hostExp);

    auto rawMixed =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToMixedName) << BSONObj());
    auto mixedExp = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), rawMixed, extension::AggStageDescriptorHandle(&_expandToMixedDescriptor));
    sources.push_back(mixedExp);

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2UL);

    Desugarer(pipeline.get())();

    // Expect [$match, extNoOp, extNoOp, $idLookup, $match].
    ASSERT_EQ(pipeline->size(), 5UL);
    auto it = pipeline->getSources().begin();
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
    ++it;
    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
    ASSERT_EQ(std::string(it->get()->getSourceName()),
              extension::sdk::shared_test_stages::kTransformName);
    ++it;
    ASSERT(dynamic_cast<DocumentSourceMatch*>(it->get()));
    ++it;
    ASSERT(dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(it->get()));
}

TEST_F(DesugarerTest, DesugaringIsIdempotentForExtensionOnlyExpansion) {
    // Create [$mock, $expandToExtAst, $mock]. expandToExtAst -> [extOptimizable].
    DocumentSourceContainer sources;
    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto raw =
        BSON(std::string(extension::sdk::shared_test_stages::kExpandToExtAstName) << BSONObj());
    auto expandable = extension::host::DocumentSourceExtensionExpandable::create(
        getExpCtx(), raw, extension::AggStageDescriptorHandle(&_expandToExtAstDescriptor));
    sources.push_back(expandable);

    sources.push_back(DocumentSourceMock::create(getExpCtx()));

    auto pipeline = Pipeline::create(std::move(sources), getExpCtx());

    // First desugar pass.
    Desugarer(pipeline.get())();

    ASSERT_EQ(pipeline->size(), 3UL);

    auto checkPipelineShape = [&](const Pipeline* p) {
        auto it = p->getSources().begin();
        ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
        ++it;
        ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable*>(it->get()));
        ASSERT_EQ(std::string(it->get()->getSourceName()),
                  extension::sdk::shared_test_stages::kTransformName);
        ++it;
        ASSERT(dynamic_cast<DocumentSourceMock*>(it->get()));
    };

    checkPipelineShape(pipeline.get());

    // Second desugar pass should be a no-op.
    Desugarer(pipeline.get())();

    ASSERT_EQ(pipeline->size(), 3UL);
    checkPipelineShape(pipeline.get());
}

}  // namespace
}  // namespace mongo
