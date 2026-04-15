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

#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Test-only stage that lifts its subpipeline to the top level when desugared. Used to verify
// desugaring order: if subpipelines are desugared before the stage expands (innermost-first),
// the lifted content will already be desugared (e.g. [$match] not [$expandToHostParse]).
static constexpr StringData kLiftSubpipelineStageName = "$liftSubpipeline"_sd;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(LiftSubpipeline);
ALLOCATE_STAGE_PARAMS_ID(liftSubpipeline, LiftSubpipelineStageParams::id);

class LiftSubpipelineLiteParsed final
    : public LiteParsedDocumentSourceNestedPipelines<LiftSubpipelineLiteParsed> {
public:
    static std::unique_ptr<LiteParsedDocumentSource> parse(const NamespaceString& nss,
                                                           const BSONElement& spec,
                                                           const LiteParserOptions& options) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << kLiftSubpipelineStageName
                              << " stage specification must be an object",
                spec.type() == BSONType::object);

        auto pipelineElem = spec.Obj()["pipeline"];
        uassert(ErrorCodes::FailedToParse,
                str::stream() << kLiftSubpipelineStageName << " must have a 'pipeline' array",
                pipelineElem && pipelineElem.type() == BSONType::array);

        auto pipeline = parsePipelineFromBSON(pipelineElem);
        auto optsCopy = options;
        optsCopy.makeSubpipelineOwned = true;
        auto liteParsedPipeline = LiteParsedPipeline(nss, pipeline, false, optsCopy);

        return std::make_unique<LiftSubpipelineLiteParsed>(
            spec, boost::none, std::move(liteParsedPipeline));
    }

    LiftSubpipelineLiteParsed(const BSONElement& spec,
                              boost::optional<NamespaceString> foreignNss,
                              LiteParsedPipeline pipeline)
        : LiteParsedDocumentSourceNestedPipelines(
              spec, std::move(foreignNss), std::vector<LiteParsedPipeline>{std::move(pipeline)}) {}

    std::unique_ptr<StageParams> getStageParams() const override {
        return std::make_unique<LiftSubpipelineStageParams>(this->getOriginalBson());
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
    }
};

}  // namespace

class LiteParsedDesugarerTest : public AggregationContextFixture {
public:
    LiteParsedDesugarerTest() : LiteParsedDesugarerTest(_nss) {}
    explicit LiteParsedDesugarerTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {
        LiteParsedDesugarer::registerStageExpander(
            extension::host::ExpandableStageParams::id,
            extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpandable::
                stageExpander);
        LiteParsedDesugarer::registerStageExpander(
            LiftSubpipelineStageParams::id,
            [](LiteParsedPipeline* pipeline, size_t index, LiteParsedDocumentSource& stage) {
                auto& subpipelines = stage.getSubPipelines();
                tassert(8084900,
                        "$liftSubpipeline must have exactly one subpipeline",
                        subpipelines.size() == 1);
                StageSpecs lifted;
                for (const auto& s : subpipelines[0].getStages()) {
                    lifted.push_back(s->clone());
                }
                return pipeline->replaceStageWith(index, std::move(lifted));
            });
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
                return extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpandable::
                    parse(descriptor, nss, spec, opts);
            };
        }();

        registerParser(stageName, std::move(parser));
    }

    void registerParser(
        const std::string& stageName,
        std::function<std::unique_ptr<LiteParsedDocumentSource>(
            const NamespaceString&, const BSONElement&, const LiteParserOptions&)> parser) {
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

    BSONObj makeLookupWithSubpipeline(const std::vector<BSONObj>& subpipelineStages) {
        BSONArrayBuilder arr;
        for (const auto& stage : subpipelineStages) {
            arr << stage;
        }
        return BSON("$lookup" << BSON("from" << "otherCollection"
                                             << "let" << BSONObj() << "pipeline" << arr.arr()
                                             << "as"
                                             << "joined"));
    }

    void assertSubpipelineStageIsMatch(LiteParsedDocumentSource* stage,
                                       size_t subpipelineIdx,
                                       size_t stageIdx) {
        auto& subpipelines = stage->getSubPipelines();
        ASSERT_LT(subpipelineIdx, subpipelines.size());
        auto& stages = subpipelines[subpipelineIdx].getStages();
        ASSERT_LT(stageIdx, stages.size());
        ASSERT(dynamic_cast<MatchLiteParsed*>(stages[stageIdx].get()));
    }

protected:
    static inline NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "lite_parsed_desugarer_test");

    // Test stage descriptors from the SDK test helpers.
    extension::sdk::ExtensionAggStageDescriptorAdapter _expandToExtAstDescriptor{
        extension::sdk::shared_test_stages::ExpandToExtAstDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _expandToExtParseDescriptor{
        extension::sdk::shared_test_stages::ExpandToExtParseDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _topDescriptor{
        extension::sdk::shared_test_stages::TopDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _expandToHostParseDescriptor{
        extension::sdk::shared_test_stages::ExpandToHostParseDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _expandToMixedDescriptor{
        extension::sdk::shared_test_stages::ExpandToMixedDescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _midADescriptor{
        extension::sdk::shared_test_stages::MidADescriptor::make()};
    extension::sdk::ExtensionAggStageDescriptorAdapter _midBDescriptor{
        extension::sdk::shared_test_stages::MidBDescriptor::make()};
    std::shared_ptr<IncrementalFeatureRolloutContext> _ifrContext =
        std::make_shared<IncrementalFeatureRolloutContext>();
};

TEST_F(LiteParsedDesugarerTest, NoopOnEmptyPipeline) {
    std::vector<BSONObj> pipelineStages;

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 0);

    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    ASSERT_EQ(lpp.getStages().size(), 0);
}

TEST_F(LiteParsedDesugarerTest, NoopOnNonExpandableStages) {
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON("$match" << BSONObj()));
    pipelineStages.push_back(BSON("$project" << BSONObj()));

    LiteParsedPipeline lpp(_nss, pipelineStages);
    ASSERT_EQ(lpp.getStages().size(), 2);

    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

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

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

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

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

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

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

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

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

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

        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

        auto& stages = lpp.getStages();
        ASSERT_EQ(stages.size(), 1);

        ASSERT(
            dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
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

            ASSERT(dynamic_cast<
                   extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
                stages[1].get()));
            ASSERT_EQ(stages[1]->getParseTimeName(),
                      extension::sdk::shared_test_stages::kTransformName);

            ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[2].get()));
        };

        // First desugar pass.
        ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));
        checkLiteParsedPipelineShape(lpp);

        // Second desugar pass should be a no-op.
        ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));
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

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
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

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 4);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[0].get()));
    ASSERT_EQ(stages[0]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafAName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafBName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafCName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
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

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Expect [$project, extNoOp, extNoOp, $match, $idLookup, $project].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 6);

    ASSERT(dynamic_cast<ProjectLiteParsed*>(stages[0].get()));

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kTransformName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kTransformName);

    ASSERT(dynamic_cast<MatchLiteParsed*>(stages[3].get()));

    ASSERT(dynamic_cast<LiteParsedInternalSearchIdLookUp*>(stages[4].get()));

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

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Expect [LeafC, LeafD, LeafA, LeafB].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 4);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[0].get()));
    ASSERT_EQ(stages[0]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafCName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[1].get()));
    ASSERT_EQ(stages[1]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafDName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[2].get()));
    ASSERT_EQ(stages[2]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafAName);

    ASSERT(dynamic_cast<extension::host::DocumentSourceExtensionOptimizable::LiteParsedExpanded*>(
        stages[3].get()));
    ASSERT_EQ(stages[3]->getParseTimeName(), extension::sdk::shared_test_stages::kLeafBName);

    unregisterParser(midAStageName);
    unregisterParser(midBStageName);
}

TEST_F(LiteParsedDesugarerTest, DesugarsSubpipelineWithExpandableStage) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$lookup] with subpipeline [$expandToHostParse]. The subpipeline should be desugared
    // to [$match].
    LiteParsedPipeline lpp(_nss, {makeLookupWithSubpipeline({BSON(extStageName << BSONObj())})});
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Main pipeline unchanged: [$lookup].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);

    // Subpipeline should be desugared: [$expandToHostParse] -> [$match].
    assertSubpipelineStageIsMatch(stages[0].get(), 0, 0);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, SkipsSubpipelineDesugaringWhenIfrContextIsNull) {
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$lookup] with subpipeline [$expandToHostParse]. With a null IFR context, subpipeline
    // desugaring is skipped, so the subpipeline should remain unchanged.
    LiteParsedPipeline lpp(_nss, {makeLookupWithSubpipeline({BSON(extStageName << BSONObj())})});
    ASSERT_EQ(lpp.getStages().size(), 1);

    // desugar returns false - no top-level expandable stage, and subpipelines are skipped.
    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp, nullptr));

    // Subpipeline unchanged: still [$expandToHostParse] (not desugared to [$match]).
    auto& subpipelines = lpp.getStages()[0]->getSubPipelines();
    ASSERT_EQ(subpipelines.size(), 1);
    auto& subpipelineStages = subpipelines[0].getStages();
    ASSERT_EQ(subpipelineStages.size(), 1);
    ASSERT_EQ(subpipelineStages[0]->getParseTimeName(), extStageName);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, NoopOnLookupSubpipelineWithNonExpandableStages) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    // Create [$lookup] with subpipeline [$match]. No expandable stages, desugar should return
    // false.
    LiteParsedPipeline lpp(_nss, {makeLookupWithSubpipeline({BSON("$match" << BSON("a" << 1))})});
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_FALSE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Subpipeline unchanged: [$match].
    assertSubpipelineStageIsMatch(lpp.getStages()[0].get(), 0, 0);
}

TEST_F(LiteParsedDesugarerTest, DesugarsSubpipelineAndTopLevelExpandableStage) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$lookup with subpipeline [$expandToHostParse], $expandToHostParse]. Both the
    // subpipeline and the top-level stage should be desugared.
    LiteParsedPipeline lpp(_nss,
                           {makeLookupWithSubpipeline({BSON(extStageName << BSONObj())}),
                            BSON(extStageName << BSONObj())});
    ASSERT_EQ(lpp.getStages().size(), 2);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Main pipeline: [$lookup, $match] (expandToHostParse expanded to $match).
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 2);
    ASSERT(dynamic_cast<MatchLiteParsed*>(stages[1].get()));

    // Subpipeline desugared: [$expandToHostParse] -> [$match].
    assertSubpipelineStageIsMatch(stages[0].get(), 0, 0);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, DesugarsAllSubpipelinesInFacetStage) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$facet] with two sub-pipelines, each containing [$expandToHostParse]. $facet has
    // multiple sub-pipelines (one per facet), so we verify each gets desugared.
    BSONObj expandStage = BSON(extStageName << BSONObj());
    LiteParsedPipeline lpp(_nss,
                           {BSON("$facet" << BSON("facetA" << BSON_ARRAY(expandStage) << "facetB"
                                                           << BSON_ARRAY(expandStage)))});
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Main pipeline unchanged: [$facet].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);

    // Both subpipelines should be desugared: each [$expandToHostParse] -> [$match].
    assertSubpipelineStageIsMatch(stages[0].get(), 0, 0);
    assertSubpipelineStageIsMatch(stages[0].get(), 1, 0);

    unregisterParser(extStageName);
}

TEST_F(LiteParsedDesugarerTest, DesugarsNestedSubpipelines) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$unionWith] whose subpipeline contains [$lookup] with its own subpipeline. This tests
    // nested subpipelines: $unionWith -> $lookup -> $expandToHostParse. The innermost expandable
    // stage should be desugared.
    BSONObj lookupStage = makeLookupWithSubpipeline({BSON(extStageName << BSONObj())});
    LiteParsedPipeline lpp(
        _nss,
        {BSON("$unionWith" << BSON("coll" << "otherCollection"
                                          << "pipeline" << BSON_ARRAY(lookupStage)))});
    ASSERT_EQ(lpp.getStages().size(), 1);

    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // Main pipeline unchanged: [$unionWith].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);

    // $unionWith's subpipeline: [$lookup]. The $lookup's subpipeline should be desugared.
    auto& unionWithSubpipelines = stages[0]->getSubPipelines();
    ASSERT_EQ(unionWithSubpipelines.size(), 1);
    auto& unionWithPipelineStages = unionWithSubpipelines[0].getStages();
    ASSERT_EQ(unionWithPipelineStages.size(), 1);

    // The $lookup stage's subpipeline (nested): [$expandToHostParse] -> [$match].
    assertSubpipelineStageIsMatch(unionWithPipelineStages[0].get(), 0, 0);

    unregisterParser(extStageName);
}

// Verifies that desugaring happens innermost-first by using a stage that lifts its subpipeline to
// the top level when it expands. If the subpipeline is desugared before the lift stage expands,
// the lifted content will be [$match] (desugared). If outermost-first, we'd lift
// [$expandToHostParse].
TEST_F(LiteParsedDesugarerTest, DesugaringOrderInnermostFirst) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtensionsInsideHybridSearch",
                                                     true};
    registerParser(std::string(kLiftSubpipelineStageName), LiftSubpipelineLiteParsed::parse);
    registerParser(extension::AggStageDescriptorHandle(&_expandToHostParseDescriptor));
    const auto extStageName =
        std::string(extension::sdk::shared_test_stages::kExpandToHostParseName);

    // Create [$liftSubpipeline { pipeline: [$expandToHostParse] }]. When the lift stage expands,
    // it moves its subpipeline to the top. With innermost-first, the subpipeline was already
    // desugared, so we lift [$match]. With outermost-first, we'd lift [$expandToHostParse].
    std::vector<BSONObj> pipelineStages;
    pipelineStages.push_back(BSON(std::string(kLiftSubpipelineStageName) << BSON(
                                      "pipeline" << BSON_ARRAY(BSON(extStageName << BSONObj())))));

    LiteParsedPipeline lpp(_nss, pipelineStages);

    ASSERT_EQ(lpp.getStages().size(), 1);
    ASSERT_TRUE(LiteParsedDesugarer::desugar(&lpp, _ifrContext));

    // The lift stage was replaced with its subpipeline contents. With innermost-first ordering,
    // those contents were desugared before the lift, so we get [$match] not [$expandToHostParse].
    auto& stages = lpp.getStages();
    ASSERT_EQ(stages.size(), 1);
    ASSERT(dynamic_cast<MatchLiteParsed*>(stages[0].get()))
        << "Expected [$match]; if desugaring were outermost-first, we'd have lifted "
           "[$expandToHostParse] before it was desugared.";

    unregisterParser(extStageName);
    unregisterParser(std::string(kLiftSubpipelineStageName));
}
}  // namespace mongo
