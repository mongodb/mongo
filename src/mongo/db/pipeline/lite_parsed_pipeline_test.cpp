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

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/test_lite_parsed.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test.liteParsedPipeline"_sd);
const NamespaceString kViewNss = NamespaceString::createNamespaceString_forTest("test.view"_sd);
const NamespaceString kResolvedNss =
    NamespaceString::createNamespaceString_forTest("test.collection"_sd);

/**
 * Helper function to create a ViewInfo with a simple pipeline.
 */
ViewInfo createTestViewInfo(std::vector<BSONObj> viewPipeline) {
    return ViewInfo(kViewNss, kResolvedNss, std::move(viewPipeline));
}

TEST(LiteParsedPipelineTest, HandleViewStitchesViewBeforeUserPipe) {
    std::vector<BSONObj> userStages = {BSON("$group" << BSON("_id" << "$field")),
                                       BSON("$sort" << BSON("_id" << 1))};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 10)};
    const auto viewInfo = createTestViewInfo(std::move(viewStages));

    pipeline.handleView(viewInfo);

    // Verify the pipeline now contains the view stages in the correct order.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 4U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$group");
    ASSERT_EQ(stages[3]->getParseTimeName(), "$sort");
}

TEST(LiteParsedPipelineTest, HandleViewEmptyUserPipelineBecomesViewPipeline) {
    // Create an empty user pipeline.
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{});

    // Create a view with two stages.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)),
                                       BSON("$project" << BSON("y" << 1))};
    const auto viewInfo = createTestViewInfo(std::move(viewStages));

    pipeline.handleView(viewInfo);

    // Final pipeline should be view pipeline.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 2U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$project");
}

TEST(LiteParsedPipelineTest, HandleViewEmptyViewPipelineIsNoop) {
    // Create a pipeline with user stages.
    std::vector<BSONObj> userStages = {BSON("$match" << BSON("x" << 1))};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    // Create an empty view pipeline.
    const auto viewInfo = createTestViewInfo({});

    pipeline.handleView(viewInfo);

    // User pipeline should be the same.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 1U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
}

TEST(LiteParsedPipelineTest, HandleViewDoesNotConsumeOrMutateViewInfo) {
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{});

    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1))};
    const auto viewInfo = createTestViewInfo(std::move(viewStages));

    ASSERT_TRUE(viewInfo.viewPipeline);
    const auto before = viewInfo.getOriginalBson();
    ASSERT_EQ(before.size(), 1U);

    pipeline.handleView(viewInfo);

    ASSERT_TRUE(viewInfo.viewPipeline);
    const auto after = viewInfo.getOriginalBson();
    ASSERT_EQ(after.size(), 1U);
    ASSERT_BSONOBJ_EQ(before[0], after[0]);
}

TEST(LiteParsedPipelineTest, HandleViewStitchedPipelineSurvivesViewInfoLifetime) {
    // User pipeline has one stage so we can see ordering.
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("x" << 1))});
    {
        // ViewInfo lives only in this scope.
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 5)};
        auto viewInfo = createTestViewInfo(std::move(viewStages));
        pipeline.handleView(viewInfo);
    }
    // View pipeline stages own their backing BSON, so they remain valid after ViewInfo is
    // destroyed. Verify the pipeline now contains the view stages in the correct order.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 3U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$sort");
}

TEST(LiteParsedPipelineTest, ViewInfoCloneIsIndependentOfOriginalLifetime) {
    ViewInfo cloned;
    {
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)),
                                           BSON("$project" << BSON("y" << 1))};
        auto viewInfo = createTestViewInfo(std::move(viewStages));
        cloned = viewInfo.clone();
    }
    ASSERT_TRUE(cloned.viewPipeline);
    LiteParsedPipeline userPipe(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("y" << 1))});

    userPipe.handleView(cloned);

    const auto& stages = userPipe.getStages();
    ASSERT_EQ(stages.size(), 3U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$project");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$sort");
}

TEST(LiteParsedPipelineTest, ClonedPipelineWithViewStagesPreservesOwnership) {
    // Create a pipeline with view stages stitched in.
    LiteParsedPipeline original(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("x" << 1))});
    {
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 5)};
        auto viewInfo = createTestViewInfo(std::move(viewStages));
        original.handleView(viewInfo);
    }

    // Clone the pipeline after ViewInfo is destroyed. The cloned stages should also own their BSON.
    auto cloned = original.clone();

    // Verify both pipelines have the correct stages.
    const auto& originalStages = original.getStages();
    const auto& clonedStages = cloned.getStages();
    ASSERT_EQ(originalStages.size(), 3U);
    ASSERT_EQ(clonedStages.size(), 3U);
    ASSERT_EQ(originalStages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(clonedStages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(originalStages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(clonedStages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(originalStages[2]->getParseTimeName(), "$sort");
    ASSERT_EQ(clonedStages[2]->getParseTimeName(), "$sort");
}

}  // namespace

// Parses { <stageName>: {} } into a TestLiteParsed whose ViewPolicy is fixed.
std::unique_ptr<LiteParsedDocumentSource> createViewPolicyDefaultParser(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    return std::make_unique<TestLiteParsed>(
        spec, ViewPolicy{.policy = ViewPolicy::kFirstStageApplicationPolicy::kDefaultPrepend});
}

std::unique_ptr<LiteParsedDocumentSource> createViewPolicyDoNothingParser(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    return std::make_unique<TestLiteParsed>(
        spec, ViewPolicy{.policy = ViewPolicy::kFirstStageApplicationPolicy::kDoNothing});
}

class LiteParsedPipelineViewPolicyTest : public unittest::Test {
protected:
    void setUp() override {
        _defaultStageName = "$viewPolicyDefault";
        _doNothingStageName = "$viewPolicyDoNothing";

        LiteParsedDocumentSource::registerParser(
            _defaultStageName,
            {.parser = createViewPolicyDefaultParser,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});

        LiteParsedDocumentSource::registerParser(
            _doNothingStageName,
            {.parser = createViewPolicyDoNothingParser,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    void tearDown() override {
        LiteParsedDocumentSource::unregisterParser_forTest(_defaultStageName);
        LiteParsedDocumentSource::unregisterParser_forTest(_doNothingStageName);
    }

    BSONObj defaultStageSpec() const {
        return BSON(_defaultStageName << BSONObj());
    }

    BSONObj doNothingStageSpec() const {
        return BSON(_doNothingStageName << BSONObj());
    }

    ViewInfo makeViewInfo() const {
        return createTestViewInfo({BSON("$match" << BSON("x" << 1))});
    }

    std::string _defaultStageName;
    std::string _doNothingStageName;
};

TEST_F(LiteParsedPipelineViewPolicyTest, FirstDoNothingSuppressesPrepend) {
    std::vector<BSONObj> userStages = {doNothingStageSpec(), defaultStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto viewInfo = makeViewInfo();
    pipeline.handleView(viewInfo);

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 2U);
    ASSERT_EQ(out[0]->getParseTimeName(), _doNothingStageName);
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
}

TEST_F(LiteParsedPipelineViewPolicyTest,
       LaterDoNothingDoesNotAffectPrependIfFirstIsDefaultPrepend) {
    std::vector<BSONObj> userStages = {defaultStageSpec(), doNothingStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto viewInfo = makeViewInfo();
    pipeline.handleView(viewInfo);

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 3U);
    ASSERT_EQ(out[0]->getParseTimeName(), "$match");
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
    ASSERT_EQ(out[2]->getParseTimeName(), _doNothingStageName);
}

TEST_F(LiteParsedPipelineViewPolicyTest, PrependWhenDefaultPrependIsTrueForAllStages) {
    std::vector<BSONObj> userStages = {defaultStageSpec(), defaultStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto viewInfo = makeViewInfo();
    pipeline.handleView(viewInfo);

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 3U);
    ASSERT_EQ(out[0]->getParseTimeName(), "$match");
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
    ASSERT_EQ(out[2]->getParseTimeName(), _defaultStageName);
}

}  // namespace mongo

