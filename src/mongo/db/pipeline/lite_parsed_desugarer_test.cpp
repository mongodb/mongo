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

#include "mongo/db/pipeline/lite_parsed_desugarer.h"

#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class LiteParsedDesugarerTest : public AggregationContextFixture {
public:
    LiteParsedDesugarerTest() : LiteParsedDesugarerTest(_nss) {}
    explicit LiteParsedDesugarerTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {
        LiteParsedDesugarer::registerStageExpander(
            extension::host::ExpandableStageParams::id,
            extension::host::DocumentSourceExtension::LiteParsedExpandable::stageExpander);
    }

    void registerParser(extension::AggStageDescriptorHandle descriptor) {
        auto nameStringData = descriptor->getName();
        auto stageName = std::string(nameStringData);

        using LiteParseFn = std::function<std::unique_ptr<LiteParsedDocumentSource>(
            const NamespaceString&, const BSONElement&, const LiteParserOptions&)>;

        auto parser = [&]() -> LiteParseFn {
            return [descriptor](const NamespaceString& nss,
                                const BSONElement& spec,
                                const LiteParserOptions& opts) {
                return extension::host::DocumentSourceExtension::LiteParsedExpandable::parse(
                    descriptor, nss, spec, opts);
            };
        }();

        LiteParsedDocumentSource::registerParser(
            stageName,
            {.parser = std::move(parser),
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    void unregisterParser(const std::string& stageName) {
        LiteParsedDocumentSource::unregisterParser_forTest(stageName);
    }

    void setUp() override {
        AggregationContextFixture::setUp();
        extension::sdk::HostServicesAPI::setHostServices(
            &extension::host_connector::HostServicesAdapter::get());
    }

protected:
    static inline NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "lite_parsed_desugarer_test");

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
    extension::sdk::ExtensionAggStageDescriptor _midADescriptor{
        extension::sdk::shared_test_stages::MidADescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptor _midBDescriptor{
        extension::sdk::shared_test_stages::MidBDescriptor::make()};
};

TEST_F(LiteParsedDesugarerTest, NoopOnEmptyPipeline) {
    std::vector<BSONObj> pipelineStages;

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 0);

    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp));

    ASSERT_EQ(lpp.getStages().size(), 0);
}

TEST_F(LiteParsedDesugarerTest, NoopOnNonExpandableStages) {
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON("$match" << BSONObj()));
    pipelineStages.push_back(BSON("$project" << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 2);

    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp));

    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 2);
    ASSERT(dynamic_cast<MatchLiteParsed*>(stages[0].get()));
    ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[1].get()));
}

TEST_F(LiteParsedDesugarerTest, ExpandsExpandableToHostParseStage) {
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    {
        // ExpandsSingleExpandToHostParseOnly
        // Create [$expandToHostParse].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON(extStageName << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 1);

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 1);
        ASSERT(dynamic_cast<MatchLiteParsed*>(stages[0].get()));
    }
    {
        // ExpandsSingleExpandableToHostStageInPlace
        // Create [$project, $expandToHostParse, $project].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON("$project" << BSONObj()));
        pipelineStages.push_back(BSON(extStageName << BSONObj()));
        pipelineStages.push_back(BSON("$project" << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 3);

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

        // Expect: [$project, $match, $project].
        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 3);
        ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[0].get()));
        ASSERT(dynamic_cast<MatchLiteParsed*>(stages[1].get()));
        ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[2].get()));
    }

    {
        // ExpandsHeadExpandableStage
        // Create [$expandToHostParse, $project].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON(extStageName << BSONObj()));
        pipelineStages.push_back(BSON("$project" << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 2);

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

        // Expect: [$match, $project].
        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 2);
        ASSERT(dynamic_cast<MatchLiteParsed*>(stages[0].get()));
        ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[1].get()));
    }

    {
        // ExpandsTailExpandableStage
        // Create [$project, $expandToHostParse].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON("$project" << BSONObj()));
        pipelineStages.push_back(BSON(extStageName << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 2);

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

        // Expect: [$project, $match].
        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 2);
        ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[0].get()));
        ASSERT(dynamic_cast<MatchLiteParsed*>(stages[1].get()));
    }

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, ExpandsExpandToExtAst) {
    registerParser(extension::AggStageDescriptorHandle(&_expandToExtAstDescriptor));
    const auto extStageName = std::string(extension::sdk::shared_test_stages::kExpandToExtAstName);

    {
        // ExpandsSingleExpandToExtAstOnly
        // Create [$expandToExtAst].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON(extStageName << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 1);

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 1);

        ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
            stages[0].get()));
        ASSERT_EQ(stages[0]->getParseTimeName(),
                  extension::sdk::shared_test_stages::kTransformName);
    }
    {
        // DesugaringIsIdempotentForExtensionOnlyExpansion
        // Create [$project, $expandToExtAst, $project]. expandToExtAst -> [extExpanded].
        std::vector<BSONObj> pipelineStages;
        pipelineStages.push_back(BSON("$project" << BSONObj()));
        pipelineStages.push_back(BSON(extStageName << BSONObj()));
        pipelineStages.push_back(BSON("$project" << BSONObj()));

        LiteParsedPipeline lpp(_nss, pipelineStages);
        ASSERT_EQ(lpp.getStages().size(), 3);

        auto checkLiteParsedPipelineShape = [&](const LiteParsedPipeline& lpp) {
            auto& stages = lpp.getStages();
            ASSERT_EQ(stages.size(), 3);

            ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[0].get()));

            ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
                stages[1].get()));
            ASSERT_EQ(stages[1]->getParseTimeName(),
                      extension::sdk::shared_test_stages::kTransformName);

            ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[2].get()));
        };

        // First desugar pass.
        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));
        checkLiteParsedPipelineShape(lpp);

        // Second desugar pass should be a no-op.
        ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp));
        checkLiteParsedPipelineShape(lpp);
    }
}

TEST_F(LiteParsedDesugarerTest, ExpandsSingleExpandToExtParseOnly) {
    registerParser(extension::AggStageDescriptorHandle(&_expandToExtParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToExtParseName);

    // Create [$expandToExtParse].
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON(extStageName << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[0].get()));
    ASSERT_EQ(stages[0]->getParseTimeName(), extension::sdk::shared_test_stages::kTransformName);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, ExpandsRecursiveTopToLeaves) {
    // Create [$top]. Top -> [MidA, MidB] -> [LeafA, LeafB, LeafC, LeafD].
    registerParser(extension::AggStageDescriptorHandle(&_topDescriptor));
    const auto extStageName = std::string(extension::sdk::shared_test_stages::kTopName);

    // Create [$expandToExtParse].
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON(extStageName << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 4);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[0].get()));
    ASSERT_EQ(stages[0]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafAName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafBName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafCName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[3].get()));
    ASSERT_EQ(stages[3]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafDName);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, ExpandsMixedToMultipleStagesSplicingIntoPipeline) {
    registerParser(extension::AggStageDescriptorHandle(&_expandToMixedDescriptor));
    const auto extStageName = std::string(extension::sdk::shared_test_stages::kExpandToMixedName);

    // Create [$project, $expandToMixed, $project]. expandToMixed -> [extNoOp, extNoOp,
    // $match].
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON("$project" << BSONObj()));
    pipelineStages.push_back(BSON(extStageName << BSONObj()));
    pipelineStages.push_back(BSON("$project" << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 3);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

    // Expect [$project, extNoOp, extNoOp, $match, $idLookup, $project].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 6);

    ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[0].get()));

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kTransformName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kTransformName);

    ASSERT(dynamic_cast<MatchLiteParsed*>(stages[3].get()));

    ASSERT(dynamic_cast<DocumentSourceInternalSearchIdLookUp::LiteParsed*>(stages[4].get()));

    ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[5].get()));

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, ExpandsMultipleExpandablesSequentially) {
    registerParser(extension::AggStageDescriptorHandle(&_midADescriptor));
    const auto midAStageName = std::string(extension::sdk::shared_test_stages::kMidAName);
    registerParser(extension::AggStageDescriptorHandle(&_midBDescriptor));
    const auto midBStageName = std::string(extension::sdk::shared_test_stages::kMidBName);

    // Create [$midB, $midA] -> [LeafC, LeafD, LeafA, LeafB].
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON(midBStageName << BSONObj()));
    pipelineStages.push_back(BSON(midAStageName << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 2);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp));

    // Expect [LeafC, LeafD, LeafA, LeafB].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 4);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[0].get()));
    ASSERT_EQ(stages[0]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafCName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafDName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafAName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtension::LiteParsedExpanded*>(
        stages[3].get()));
    ASSERT_EQ(stages[3]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafBName);

    unregisterParser(midAStageName);
    unregisterParser(midBStageName);
}
}  // namespace mongo
