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

#include "mongo/db/pipeline/pipeline_factory.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <mutex>

namespace mongo::pipeline_factory {
namespace {

using MakePipelineBSONElementTest = AggregationContextFixture;

using MakePipelineBSONElementTestDeathTest = MakePipelineBSONElementTest;
DEATH_TEST_F(MakePipelineBSONElementTestDeathTest,
             TassertFailsWhenBSONElementIsNotArray,
             "11524600") {
    auto expCtx = getExpCtx();
    BSONObj cmdObj = BSON("pipeline" << 123);  // Not an array
    BSONElement pipelineElem = cmdObj["pipeline"];

    pipeline_factory::makePipeline(pipelineElem, expCtx, {.attachCursorSource = false});
}

TEST_F(MakePipelineBSONElementTest, UassertFailsWhenArrayElementIsNotObject) {
    auto expCtx = getExpCtx();
    BSONObj cmdObj = BSON("pipeline" << BSON_ARRAY(123 << "string" << true));
    BSONElement pipelineElem = cmdObj["pipeline"];

    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipelineElem, expCtx, {.attachCursorSource = false}),
        AssertionException,
        11524601);
}

TEST_F(MakePipelineBSONElementTest, UassertFailsWhenArrayContainsNonObject) {
    auto expCtx = getExpCtx();
    BSONObj cmdObj = BSON("pipeline" << BSON_ARRAY(BSON("$match" << BSONObj()) << 123));
    BSONElement pipelineElem = cmdObj["pipeline"];

    ASSERT_THROWS_CODE(
        pipeline_factory::makePipeline(pipelineElem, expCtx, {.attachCursorSource = false}),
        AssertionException,
        11524601);
}

TEST_F(MakePipelineBSONElementTest, SuccessfullyParsesValidArray) {
    auto expCtx = getExpCtx();
    BSONObj cmdObj =
        BSON("pipeline" << BSON_ARRAY(BSON("$match" << BSONObj()) << BSON("$limit" << 10)));
    BSONElement pipelineElem = cmdObj["pipeline"];

    auto pipeline = pipeline_factory::makePipeline(
        pipelineElem,
        expCtx,
        {.optimize = false, .alreadyOptimized = false, .attachCursorSource = false});

    auto stages = pipeline->getSources();
    ASSERT_EQ(stages.size(), 2);
    ASSERT_EQ(StringData(stages.front()->getSourceName()), DocumentSourceMatch::kStageName);
    ASSERT_EQ(StringData(stages.back()->getSourceName()), DocumentSourceLimit::kStageName);
}

// Two distinct lite-parsed types so the test can tell which parser the registry picked.
DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(IfrViewMockPrimary);
DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(IfrViewMockFallback);

std::unique_ptr<LiteParsedDocumentSource> primaryViewMockParse(const NamespaceString&,
                                                               const BSONElement& spec,
                                                               const LiteParserOptions&) {
    return std::make_unique<IfrViewMockPrimaryLiteParsed>(spec);
}

std::unique_ptr<LiteParsedDocumentSource> fallbackViewMockParse(const NamespaceString&,
                                                                const BSONElement& spec,
                                                                const LiteParserOptions&) {
    return std::make_unique<IfrViewMockFallbackLiteParsed>(spec);
}

constexpr auto kIfrViewMockStageName = "$mockGatedViewStageForIfrTest";

IncrementalRolloutFeatureFlag& getMockGatedViewStageFlag() {
    static IncrementalRolloutFeatureFlag flag{
        "featureFlagMockGatedViewStageForIfrTest"_sd, RolloutPhase::inDevelopment, true};
    static std::once_flag registered;
    std::call_once(registered, [] { flag.registerFlag(); });
    return flag;
}

// The parser registry doesn't support re-registration, so register exactly once even
// though the fixture's setUp() may run multiple times.
void ensureMockGatedViewStageRegistered() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        auto& flag = getMockGatedViewStageFlag();
        LiteParsedDocumentSource::registerFallbackParser(
            kIfrViewMockStageName,
            &flag,
            {.parser = fallbackViewMockParse,
             .allowedWithApiStrict = AllowedWithApiStrict::kNeverInVersion1,
             .allowedWithClientType = AllowedWithClientType::kInternal});
        LiteParsedDocumentSource::registerParser(
            kIfrViewMockStageName,
            {.parser = primaryViewMockParse,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    });
}

class MakePipelineFromViewDefinitionIfrTest : public AggregationContextFixture {
protected:
    void setUp() override {
        ensureMockGatedViewStageRegistered();
    }
};

TEST_F(MakePipelineFromViewDefinitionIfrTest,
       SearchOnViewForwardsIfrContextWhenParsingViewPipeline) {
    NamespaceString viewNss = NamespaceString::createNamespaceString_forTest("testdb", "viewColl");
    NamespaceString backingNss =
        NamespaceString::createNamespaceString_forTest("testdb", "backingColl");

    auto ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(std::vector<BSONObj>{
        BSON("name" << getMockGatedViewStageFlag().getName() << "value" << false)});

    auto expCtx =
        ExpressionContextBuilder{}.opCtx(getOpCtx()).ns(viewNss).ifrContext(ifrContext).build();

    // $listSearchIndexes makes isMongotPipeline() return true, routing through
    // viewPipelineHelperForSearch.
    std::vector<BSONObj> viewPipeline = {fromjson("{$mockGatedViewStageForIfrTest: {}}")};
    std::vector<BSONObj> userPipeline = {fromjson("{$listSearchIndexes: {}}")};

    auto pipeline =
        makePipelineFromViewDefinition(expCtx,
                                       ResolvedNamespace{backingNss, std::move(viewPipeline)},
                                       std::move(userPipeline),
                                       kOptionsMinimal,
                                       viewNss);

    ASSERT_TRUE(expCtx->getView().has_value());
    auto viewLpp = expCtx->getView()->getViewPipeline();
    ASSERT_EQ(viewLpp.getStages().size(), 1u);
    ASSERT_TRUE(dynamic_cast<IfrViewMockFallbackLiteParsed*>(viewLpp.getStages().front().get()))
        << "IFR context not forwarded - primary parser picked instead of fallback.";
}

ALLOCATE_STAGE_PARAMS_ID(ifrViewMockPrimary, IfrViewMockPrimaryStageParams::id);
ALLOCATE_STAGE_PARAMS_ID(ifrViewMockFallback, IfrViewMockFallbackStageParams::id);

}  // namespace
}  // namespace mongo::pipeline_factory

