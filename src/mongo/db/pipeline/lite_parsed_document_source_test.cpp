/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_document_source.h"

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/pipeline/test_lite_parsed.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test.liteParsedDocSource"_sd);

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Mock);
ALLOCATE_STAGE_PARAMS_ID(mock, MockStageParams::id);

// Mock parser functions for testing
std::unique_ptr<LiteParsedDocumentSource> createMockParser(const NamespaceString& nss,
                                                           const BSONElement& spec,
                                                           const LiteParserOptions& options) {
    return std::make_unique<MockLiteParsed>(spec);
}

}  // namespace

class LiteParserRegistrationTest : public unittest::Test {
protected:
    void setUp() override {
        primaryParser = {.parser = createMockParser,
                         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
                         .allowedWithClientType = AllowedWithClientType::kAny};
        fallbackParser = {.parser = createMockParser,
                          .allowedWithApiStrict = AllowedWithApiStrict::kNeverInVersion1,
                          .allowedWithClientType = AllowedWithClientType::kInternal};
    }

    void assertParserIsPrimary(const LiteParsedDocumentSource::LiteParserInfo& parserInfo) {
        ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kAlways);
        ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kAny);
    }

    void assertParserIsFallback(const LiteParsedDocumentSource::LiteParserInfo& parserInfo) {
        ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
        ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
    }

    std::pair<LiteParsedDocumentSource::LiteParserRegistration, LiteParserOptions>
    makeRegistrationWithOptions(IncrementalRolloutFeatureFlag& mockFlag,
                                const boost::optional<bool> ifrFlagValue = boost::none) {
        LiteParsedDocumentSource::LiteParserRegistration registration;
        mockFlag.registerFlag();
        registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
        registration.setPrimaryParser(std::move(primaryParser));
        LiteParserOptions options;
        if (ifrFlagValue) {
            std::vector<BSONObj> flagValues{
                BSON("name" << mockFlag.getName() << "value" << *ifrFlagValue)};
            options.ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(flagValues);
        }
        return {std::move(registration), std::move(options)};
    }

    LiteParsedDocumentSource::LiteParserInfo primaryParser;
    LiteParsedDocumentSource::LiteParserInfo fallbackParser;
};

TEST_F(LiteParserRegistrationTest, SetPrimaryParser) {
    LiteParsedDocumentSource::LiteParserRegistration registration;
    ASSERT_FALSE(registration.isPrimarySet());
    registration.setPrimaryParser(std::move(primaryParser));
    ASSERT_TRUE(registration.isPrimarySet());
}

TEST_F(LiteParserRegistrationTest, SetFallbackParser) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    // Create a mock IncrementalRolloutFeatureFlag.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);

    // Verify primary is still not set.
    ASSERT_FALSE(registration.isPrimarySet());
    ASSERT_TRUE(registration.isFallbackSet());
}

TEST_F(LiteParserRegistrationTest, GetParserWithoutFeatureFlag) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    registration.setPrimaryParser(std::move(primaryParser));

    // When there's no feature flag, should return primary parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsPrimary(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithFeatureFlagEnabled) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, true);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is enabled, should return primary parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsPrimary(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithFeatureFlagDisabled) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is disabled, should return fallback parser.
    const auto& parserInfo = registration.getParserInfo();
    assertParserIsFallback(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithChangingFeatureFlag) {
    LiteParsedDocumentSource::LiteParserRegistration registration;

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registration.setFallbackParser(std::move(fallbackParser), &mockFlag);
    registration.setPrimaryParser(std::move(primaryParser));

    // When feature flag is disabled, should return fallback parser.
    assertParserIsFallback(registration.getParserInfo());

    // Enable the feature flag and check that the primary parser is chosen.
    mockFlag.setForServerParameter(true);
    assertParserIsPrimary(registration.getParserInfo());

    // Disable it and check that the fallback parser is chosen.
    mockFlag.setForServerParameter(false);
    assertParserIsFallback(registration.getParserInfo());
}

TEST_F(LiteParserRegistrationTest, GetParserWithIfrContextFlagEnabled) {
    static IncrementalRolloutFeatureFlag mockFlag(
        "testFlag1"_sd, RolloutPhase::inDevelopment, false);
    const auto& [registration, options] = makeRegistrationWithOptions(mockFlag, true);

    // Should return primary parser because ifrContext overrides to enabled.
    const auto& parserInfo = registration.getParserInfo(options);
    assertParserIsPrimary(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithIfrContextFlagDisabled) {
    static IncrementalRolloutFeatureFlag mockFlag(
        "testFlag2"_sd, RolloutPhase::inDevelopment, true);
    const auto& [registration, options] = makeRegistrationWithOptions(mockFlag, false);

    // Should return fallback parser because ifrContext overrides to disabled.
    const auto& parserInfo = registration.getParserInfo(options);
    assertParserIsFallback(parserInfo);
}

TEST_F(LiteParserRegistrationTest, GetParserWithEmptyIfrContextFlag) {
    static IncrementalRolloutFeatureFlag mockFlag(
        "testFlag3"_sd, RolloutPhase::inDevelopment, true);
    const auto& [registration, options] = makeRegistrationWithOptions(mockFlag);

    // Should use checkEnabled() and return fallback parser.
    const auto& parserInfo = registration.getParserInfo(options);
    assertParserIsPrimary(parserInfo);
}

class LiteParsedDocumentSourceParseTest : public unittest::Test {
protected:
    void registerPrimaryParser() {
        LiteParsedDocumentSource::registerParser(
            _stageName,
            {.parser = createMockParser,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    void registerFallbackParser(FeatureFlag* ff) {
        LiteParsedDocumentSource::registerFallbackParser(
            _stageName,
            ff,
            {.parser = createMockParser,
             .allowedWithApiStrict = AllowedWithApiStrict::kNeverInVersion1,
             .allowedWithClientType = AllowedWithClientType::kInternal});
    }

    void tearDown() override {
        LiteParsedDocumentSource::unregisterParser_forTest(_stageName);
    }

    const LiteParsedDocumentSource::LiteParserInfo& getParserInfo() {
        return LiteParsedDocumentSource::getParserInfo_forTest(_stageName);
    }

    // Note that _stageName should be unique between every test and set at the beginning. This is
    // because we have no way of clearing out the global variable aggStageCounters.
    std::string _stageName;
};

TEST_F(LiteParsedDocumentSourceParseTest, CanRegisterBothPrimaryAndFallback) {
    _stageName = "$canRegisterBothPrimaryAndFallback";

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);
    registerPrimaryParser();
}

using LiteParsedDocumentSourceParseDeathTest = LiteParsedDocumentSourceParseTest;

DEATH_TEST_F(LiteParsedDocumentSourceParseDeathTest, MustRegisterPrimaryAfterFallback, "11395100") {
    _stageName = "$mustRegisterPrimaryAfterFallback";

    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerPrimaryParser();
    registerFallbackParser(&mockFlag);
}

DEATH_TEST_F(LiteParsedDocumentSourceParseDeathTest, CannotOverridePrimaryParser, "11534800") {
    _stageName = "$cannotOverridePrimaryParser";

    // Register the primary parser first.
    registerPrimaryParser();

    // Try to register another primary parser for the same stage, which should assert.
    registerPrimaryParser();
}

TEST_F(LiteParsedDocumentSourceParseTest, FirstFallbackParserTakesPrecedence) {
    _stageName = "$firstFallbackParserTakesPrecedence";

    // Disable the feature flag such that the parser returns the fallback parser.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);
    registerPrimaryParser();

    // Try creating another fallback parser.
    LiteParsedDocumentSource::registerFallbackParser(
        _stageName,
        &mockFlag,
        {.parser = createMockParser,
         .allowedWithApiStrict = AllowedWithApiStrict::kNeverInVersion1,
         .allowedWithClientType = AllowedWithClientType::kAny});

    // Ensure that the parser info is the original fallback parser.
    auto parserInfo = getParserInfo();
    ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
    ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
}

TEST_F(LiteParsedDocumentSourceParseTest, FirstFallbackParserTakesPrecedenceWithNoPrimary) {
    _stageName = "$firstFallbackParserTakesPrecedenceWithNoPrimary";

    // Disable the feature flag such that the parser returns the fallback parser.
    IncrementalRolloutFeatureFlag mockFlag("testFlag"_sd, RolloutPhase::inDevelopment, false);
    registerFallbackParser(&mockFlag);

    // Try creating another fallback parser.
    LiteParsedDocumentSource::registerFallbackParser(
        _stageName,
        &mockFlag,
        {.parser = createMockParser,
         .allowedWithApiStrict = AllowedWithApiStrict::kNeverInVersion1,
         .allowedWithClientType = AllowedWithClientType::kAny});

    // Ensure that the parser info is the original fallback parser.
    auto parserInfo = getParserInfo();
    ASSERT_EQ(parserInfo.allowedWithApiStrict, AllowedWithApiStrict::kNeverInVersion1);
    ASSERT_EQ(parserInfo.allowedWithClientType, AllowedWithClientType::kInternal);
}

// TODO SERVER-114028 Remove the following test when fallback parsing supports all feature flags.
DEATH_TEST_F(LiteParsedDocumentSourceParseDeathTest, IFRFlagIsRequired, "11395101") {
    _stageName = "$IFRFlagIsRequired";

    BinaryCompatibleFeatureFlag mockFlag(false);
    registerFallbackParser(&mockFlag);
}

// Allocate StageParams ID for TestLiteParsed.
ALLOCATE_STAGE_PARAMS_ID(test, TestStageParams::id);

/**
 * Verifies that LiteParsedDocumentSource can return StageParams via getStageParams().
 *
 * This test ensures that:
 *   1. getStageParams() returns a valid StageParams object
 *   2. The returned object is of the correct derived type (TestStageParams in this case)
 *   3. The StageParams has a properly allocated, non-zero ID
 *
 * This validates the integration between LiteParsedDocumentSource and the StageParams
 * type system.
 */
TEST(StageParams, CanGetStageParamsFromLiteParsed) {
    BSONObj spec = BSON("$testStage" << BSONObj());
    auto lp = TestLiteParsed(spec.firstElement());
    auto baseParams = lp.getStageParams();
    ASSERT_TRUE(dynamic_cast<TestStageParams*>(baseParams.get()) != nullptr);
    ASSERT_NE(baseParams->getId(), 0);
}

TEST(ViewPolicy, CanSpecifyDefaultViewPolicyWithNoCustomReadFunction) {
    BSONObj spec = BSON("$testStage" << BSONObj());
    auto lp = TestLiteParsed(spec.firstElement());

    ASSERT_EQ(lp.getViewPolicy().policy, ViewPolicy::kFirstStageApplicationPolicy::kDefaultPrepend);
}

TEST(ViewPolicy, CanSpecifyViewPolicyWithCustomReadFunction) {
    // Make a custom view function that just sets a bool flag indicating that the function has been
    // called successfully.
    bool fired = false;
    auto viewFunc = [&fired](const ViewInfo&, StringData stageName) {
        ASSERT_FALSE(fired);
        fired = true;
    };

    BSONObj spec = BSON("$testStage" << BSONObj());
    auto lp =
        TestLiteParsed(spec.firstElement(),
                       ViewPolicy{.policy = ViewPolicy::kFirstStageApplicationPolicy::kDoNothing,
                                  .callback = viewFunc});

    auto viewPolicy = lp.getViewPolicy();
    ASSERT_EQ(viewPolicy.policy, ViewPolicy::kFirstStageApplicationPolicy::kDoNothing);

    // Make sure that we can actually call the custom view function.
    viewPolicy.callback({}, "");
    ASSERT_TRUE(fired);
}

TEST(ViewPolicy, CanSpecifyDisallowViewPolicyDefaultValues) {
    BSONObj spec = BSON("$testStage" << BSONObj());
    auto lp = TestLiteParsed(spec.firstElement(), DisallowViewsPolicy{});

    auto viewPolicy = lp.getViewPolicy();
    ASSERT_EQ(viewPolicy.policy, ViewPolicy::kFirstStageApplicationPolicy::kDoNothing);

    ASSERT_THROWS_CODE_AND_WHAT(viewPolicy.callback({}, "$test"),
                                DBException,
                                ErrorCodes::CommandNotSupportedOnView,
                                "$test is not supported on views.");
}

TEST(ViewPolicy, CanSpecifyDisallowViewPolicyCustomValues) {
    BSONObj spec = BSON("$testStage" << BSONObj());

    const int kCustomErrorCode = 11505700;
    const std::string kCustomErrorMsg = "test error message";
    auto lp = TestLiteParsed(spec.firstElement(), DisallowViewsPolicy{[&](const auto&, auto) {
                                 uasserted(kCustomErrorCode, kCustomErrorMsg);
                             }});

    auto viewPolicy = lp.getViewPolicy();
    ASSERT_EQ(viewPolicy.policy, ViewPolicy::kFirstStageApplicationPolicy::kDoNothing);

    ASSERT_THROWS_CODE_AND_WHAT(
        viewPolicy.callback({}, "$test"), DBException, kCustomErrorCode, kCustomErrorMsg);
}

TEST(LiteParsedPipelineClone, CloneCreatesDeepCopy) {
    // Create a pipeline with a simple stage.
    std::vector<BSONObj> pipelineStages = {
        BSON("$match" << BSON("x" << 1)),
        BSON("$project" << BSON("y" << 1)),
    };
    LiteParsedPipeline original(kTestNss, pipelineStages);

    // Clone the pipeline.
    LiteParsedPipeline cloned = original.clone();

    // Verify both pipelines have the same number of stages.
    ASSERT_EQ(original.getStages().size(), cloned.getStages().size());
    ASSERT_EQ(original.getStages().size(), 2);

    // Verify the stages are different objects (deep copy).
    for (size_t i = 0; i < original.getStages().size(); ++i) {
        ASSERT_NE(original.getStages()[i].get(), cloned.getStages()[i].get());
    }

    // Verify that the stages have the same parse time names.
    for (size_t i = 0; i < original.getStages().size(); ++i) {
        ASSERT_EQ(original.getStages()[i]->getParseTimeName(),
                  cloned.getStages()[i]->getParseTimeName());
    }
}

TEST(LiteParsedPipelineClone, ClonedPipelineIsIndependent) {
    // Create a pipeline with stages.
    std::vector<BSONObj> pipelineStages = {
        BSON("$match" << BSON("x" << 1)),
    };
    LiteParsedPipeline original(kTestNss, pipelineStages);

    // Clone the pipeline.
    LiteParsedPipeline cloned = original.clone();

    // Verify they have the same properties before any modification.
    ASSERT_EQ(original.getStages().size(), cloned.getStages().size());

    // The original and cloned pipelines should be completely independent.
    // After cloning, the two vectors should have different underlying storage.
    ASSERT_NE(&original.getStages(), &cloned.getStages());
}

TEST(LiteParsedPipelineClone, CloneClonesSubpipelinesForNestedStages) {
    // Create a pipeline with a $lookup stage that has a subpipeline.
    std::vector<BSONObj> pipelineStages = {
        BSON("$lookup" << BSON("from" << "otherCollection"
                                      << "let" << BSONObj() << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("a" << 1))) << "as"
                                      << "joined")),
    };
    LiteParsedPipeline original(kTestNss, pipelineStages);

    // Clone the pipeline.
    LiteParsedPipeline cloned = original.clone();

    // Verify both have the $lookup stage.
    ASSERT_EQ(original.getStages().size(), 1);
    ASSERT_EQ(cloned.getStages().size(), 1);

    // Verify the stages are different objects.
    ASSERT_NE(original.getStages()[0].get(), cloned.getStages()[0].get());

    // Verify both stages have subpipelines.
    const auto& originalSubPipelines = original.getStages()[0]->getSubPipelines();
    const auto& clonedSubPipelines = cloned.getStages()[0]->getSubPipelines();

    ASSERT_EQ(originalSubPipelines.size(), 1);
    ASSERT_EQ(clonedSubPipelines.size(), 1);

    // Verify the subpipelines are different objects (deep copy).
    ASSERT_NE(&originalSubPipelines[0], &clonedSubPipelines[0]);

    // Verify the subpipeline stages are also cloned (different pointers).
    ASSERT_EQ(originalSubPipelines[0].getStages().size(), 1);
    ASSERT_EQ(clonedSubPipelines[0].getStages().size(), 1);
    ASSERT_NE(originalSubPipelines[0].getStages()[0].get(),
              clonedSubPipelines[0].getStages()[0].get());
}

TEST(LiteParsedPipelineClone, DeferredCachesAreResetInClonedPipeline) {
    // Create a pipeline that involves a foreign namespace to populate the deferred cache.
    std::vector<BSONObj> pipelineStages = {
        BSON("$lookup" << BSON("from" << "foreignCollection"
                                      << "localField"
                                      << "x"
                                      << "foreignField"
                                      << "y"
                                      << "as"
                                      << "joined")),
    };
    LiteParsedPipeline original(kTestNss, pipelineStages);

    // Access the involved namespaces to populate the deferred cache in the original.
    const auto& originalNamespaces = original.getInvolvedNamespaces();
    ASSERT_EQ(originalNamespaces.size(), 1);

    // Clone the pipeline.
    LiteParsedPipeline cloned = original.clone();

    // Access the involved namespaces in the clone.
    const auto& clonedNamespaces = cloned.getInvolvedNamespaces();
    ASSERT_EQ(clonedNamespaces.size(), 1);

    // Both should contain the same namespace.
    NamespaceString expectedNss =
        NamespaceString::createNamespaceString_forTest(kTestNss.dbName(), "foreignCollection");
    ASSERT_TRUE(originalNamespaces.count(expectedNss) > 0);
    ASSERT_TRUE(clonedNamespaces.count(expectedNss) > 0);

    // The cache results should be at different memory locations (independent caches).
    ASSERT_NE(&originalNamespaces, &clonedNamespaces);
}

TEST(LiteParsedPipelineClone, ClonedPipelineHasChangeStreamCacheReset) {
    // Create a pipeline without change stream.
    std::vector<BSONObj> pipelineStages = {
        BSON("$match" << BSON("x" << 1)),
    };
    LiteParsedPipeline original(kTestNss, pipelineStages);

    // Access hasChangeStream to populate the deferred cache.
    ASSERT_FALSE(original.hasChangeStream());

    // Clone the pipeline.
    LiteParsedPipeline cloned = original.clone();

    // The clone should correctly report no change stream (cache was reset and recomputed).
    ASSERT_FALSE(cloned.hasChangeStream());
}

TEST(LiteParsedPipelineClone, CloneRemainsValidAfterOriginalIsDestroyed) {
    // Create a pipeline with a $lookup stage that has a subpipeline.
    std::vector<BSONObj> pipelineStages = {
        BSON("$lookup" << BSON("from" << "otherCollection"
                                      << "let" << BSONObj() << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("a" << 1))) << "as"
                                      << "joined")),
    };

    std::unique_ptr<LiteParsedPipeline> cloned;
    {
        // Create original in an inner scope.
        LiteParsedPipeline original(kTestNss, pipelineStages);

        // Clone the pipeline.
        cloned = std::make_unique<LiteParsedPipeline>(original.clone());

        // Verify clone was created successfully while original exists.
        ASSERT_EQ(cloned->getStages().size(), 1);
    }
    // Original is now destroyed.

    // Verify the clone is still fully valid after the original is destroyed.
    ASSERT_EQ(cloned->getStages().size(), 1);

    // Verify the stage data is still accessible.
    const auto& clonedSubPipelines = cloned->getStages()[0]->getSubPipelines();
    ASSERT_EQ(clonedSubPipelines.size(), 1);
    ASSERT_EQ(clonedSubPipelines[0].getStages().size(), 1);

    // Verify we can access computed properties (exercises the deferred caches).
    const auto& involvedNamespaces = cloned->getInvolvedNamespaces();
    ASSERT_EQ(involvedNamespaces.size(), 1);
}

TEST(LiteParsedPipelineClone, OriginalRemainsValidAfterCloneIsDestroyed) {
    // Create a pipeline with a $lookup stage that has a subpipeline.
    std::vector<BSONObj> pipelineStages = {
        BSON("$lookup" << BSON("from" << "otherCollection"
                                      << "let" << BSONObj() << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("a" << 1))) << "as"
                                      << "joined")),
    };

    LiteParsedPipeline original(kTestNss, pipelineStages);

    {
        // Clone the pipeline in an inner scope.
        LiteParsedPipeline cloned = original.clone();

        // Verify clone was created successfully.
        ASSERT_EQ(cloned.getStages().size(), 1);
    }
    // Clone is now destroyed.

    // Verify the original is still fully valid after the clone is destroyed.
    ASSERT_EQ(original.getStages().size(), 1);

    // Verify the stage data is still accessible.
    const auto& originalSubPipelines = original.getStages()[0]->getSubPipelines();
    ASSERT_EQ(originalSubPipelines.size(), 1);
    ASSERT_EQ(originalSubPipelines[0].getStages().size(), 1);

    // Verify we can access computed properties (exercises the deferred caches).
    const auto& involvedNamespaces = original.getInvolvedNamespaces();
    ASSERT_EQ(involvedNamespaces.size(), 1);
}

//
// Tests for ExtensionMetrics.
//

// Define a mock extension stage for testing ExtensionMetrics.
DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(MockExtensionForMetrics);
ALLOCATE_STAGE_PARAMS_ID(mockExtensionForMetrics, MockExtensionForMetricsStageParams::id);

class ExtensionMetricsTest : public unittest::Test {
protected:
    struct MockMetricResults {
        long long successDelta;
        long long failureDelta;
    };

    void tearDown() override {
        // Clean up registered parsers after each test.
        LiteParsedDocumentSource::unregisterParser_forTest(_extensionStageName);
        LiteParsedDocumentSource::unregisterParser_forTest(_nonExtensionStageName);
    }

    void registerExtensionParser() {
        LiteParsedDocumentSource::registerParser(
            _extensionStageName,
            {.parser = MockExtensionForMetricsLiteParsed::parse,
             .fromExtension = true,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    void registerNonExtensionParser() {
        LiteParsedDocumentSource::registerParser(
            _nonExtensionStageName,
            {.parser = MockExtensionForMetricsLiteParsed::parse,
             .fromExtension = false,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    /**
     * Helper to parse a pipeline with extension metrics tracking. Returns the change in
     * success and failure counters after the ExtensionMetrics object is destroyed.
     */
    MockMetricResults parsePipelineAndGetCounterDeltas(const ExtensionMetricsAllocation& allocation,
                                                       const std::vector<BSONObj>& pipelineStages,
                                                       bool markSuccess) {

        const auto successBefore = allocation.successMetricCounter->get();
        const auto failedBefore = allocation.failedMetricCounter->get();

        {
            ExtensionMetrics metrics(allocation);
            LiteParserOptions options;
            options.extensionMetrics = &metrics;

            LiteParsedPipeline pipeline(kTestNss, pipelineStages, false, options);

            if (markSuccess) {
                metrics.markSuccess();
            }
        }
        // Metrics destroyed here, counters updated.

        return {allocation.successMetricCounter->get() - successBefore,
                allocation.failedMetricCounter->get() - failedBefore};
    }

    MockMetricResults mockSuccess(const ExtensionMetricsAllocation& allocation,
                                  const std::vector<BSONObj>& pipeline) {
        return parsePipelineAndGetCounterDeltas(allocation, pipeline, true /* markSuccess */);
    }

    MockMetricResults mockFailure(const ExtensionMetricsAllocation& allocation,
                                  const std::vector<BSONObj>& pipeline) {
        return parsePipelineAndGetCounterDeltas(allocation, pipeline, false /* markSuccess */);
    }

    /**
     * Helper to create an ExtensionMetricsAllocation with a unique command name.
     * Uses the test counter to ensure unique names across tests.
     */
    ExtensionMetricsAllocation makeAllocation() {
        return ExtensionMetricsAllocation(
            "extensionMetricsTestCmd" + std::to_string(_testCounter++), ClusterRole::None);
    }

    const std::string _extensionStageName = "$testExtensionMetrics";
    const std::string _nonExtensionStageName = "$testNonExtensionMetrics";

private:
    static int _testCounter;
};

int ExtensionMetricsTest::_testCounter = 0;

TEST_F(ExtensionMetricsTest, NonExtensionStageDoesNotTrackUsedExtensions) {
    registerNonExtensionParser();
    auto allocation = makeAllocation();

    auto mockDeltas = mockSuccess(allocation, {BSON("$testNonExtensionMetrics" << BSONObj())});

    // Non-extension stages should not affect counters.
    ASSERT_EQ(mockDeltas.successDelta, 0);
    ASSERT_EQ(mockDeltas.failureDelta, 0);
}

TEST_F(ExtensionMetricsTest, ExtensionStageSuccessIncrementsSuccessCounter) {
    registerExtensionParser();
    auto allocation = makeAllocation();

    auto mockDeltas = mockSuccess(allocation, {BSON("$testExtensionMetrics" << BSONObj())});

    ASSERT_EQ(mockDeltas.successDelta, 1);
    ASSERT_EQ(mockDeltas.failureDelta, 0);
}

TEST_F(ExtensionMetricsTest, ExtensionStageFailureIncrementsFailureCounter) {
    registerExtensionParser();
    auto allocation = makeAllocation();

    auto mockDeltas = mockFailure(allocation, {BSON("$testExtensionMetrics" << BSONObj())});

    ASSERT_EQ(mockDeltas.successDelta, 0);
    ASSERT_EQ(mockDeltas.failureDelta, 1);
}

TEST_F(ExtensionMetricsTest, MixedPipelineWithExtensionStageTracksExtension) {
    registerExtensionParser();
    auto allocation = makeAllocation();

    std::vector<BSONObj> mixedPipeline = {
        BSON("$match" << BSON("x" << 1)),
        BSON("$testExtensionMetrics" << BSONObj()),
        BSON("$limit" << 10),
    };

    auto mockDeltas = mockSuccess(allocation, mixedPipeline);

    // Extension was used in pipeline, so success counter should increment.
    ASSERT_EQ(mockDeltas.successDelta, 1);
    ASSERT_EQ(mockDeltas.failureDelta, 0);
}

TEST_F(ExtensionMetricsTest, PipelineWithOnlyBuiltInStagesDoesNotTrackExtension) {
    // Don't register any extension parsers for this test.
    auto allocation = makeAllocation();

    std::vector<BSONObj> builtInOnlyPipeline = {
        BSON("$match" << BSON("x" << 1)),
        BSON("$limit" << 10),
    };

    auto mockDeltas = mockSuccess(allocation, builtInOnlyPipeline);

    // No extension was used, so neither counter should change.
    ASSERT_EQ(mockDeltas.successDelta, 0);
    ASSERT_EQ(mockDeltas.failureDelta, 0);
}

TEST_F(ExtensionMetricsTest, NullExtensionMetricsDoesNotCrash) {
    registerExtensionParser();

    // Parse without any extension metrics tracker (extensionMetrics = nullptr).
    LiteParserOptions options;
    // options.extensionMetrics is nullptr by default.

    // Parse a pipeline with an extension stage - should not crash.
    std::vector<BSONObj> pipelineStages = {BSON("$testExtensionMetrics" << BSONObj())};
    LiteParsedPipeline pipeline(kTestNss, pipelineStages, false, options);

    // Verify the stage was parsed successfully.
    ASSERT_EQ(pipeline.getStages().size(), 1);
}

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(MockForIsExtensionStage);
ALLOCATE_STAGE_PARAMS_ID(mockForIsExtensionStage, MockForIsExtensionStageStageParams::id);

class IsRegisteredExtensionStageTest : public unittest::Test {
protected:
    void tearDown() override {
        LiteParsedDocumentSource::unregisterParser_forTest(_extensionStageName);
        LiteParsedDocumentSource::unregisterParser_forTest(_nonExtensionStageName);
    }

    void registerExtensionParser() {
        LiteParsedDocumentSource::registerParser(
            _extensionStageName,
            {.parser = MockForIsExtensionStageLiteParsed::parse,
             .fromExtension = true,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    void registerNonExtensionParser() {
        LiteParsedDocumentSource::registerParser(
            _nonExtensionStageName,
            {.parser = MockForIsExtensionStageLiteParsed::parse,
             .fromExtension = false,
             .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
             .allowedWithClientType = AllowedWithClientType::kAny});
    }

    const std::string _extensionStageName = "$testExtensionStage";
    const std::string _nonExtensionStageName = "$testNonExtensionStage";
};

TEST_F(IsRegisteredExtensionStageTest, ReturnsFalseForUnregisteredStage) {
    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage("$unregisteredStage"_sd));
}

TEST_F(IsRegisteredExtensionStageTest, ReturnsFalseForNonExtensionStage) {
    registerNonExtensionParser();

    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage(_nonExtensionStageName));
}

TEST_F(IsRegisteredExtensionStageTest, ReturnsTrueForExtensionStage) {
    registerExtensionParser();

    ASSERT_TRUE(LiteParsedDocumentSource::isRegisteredExtensionStage(_extensionStageName));
}

TEST_F(IsRegisteredExtensionStageTest,
       CorrectlyDistinguishesBetweenExtensionAndNonExtensionStages) {
    registerExtensionParser();
    registerNonExtensionParser();

    ASSERT_TRUE(LiteParsedDocumentSource::isRegisteredExtensionStage(_extensionStageName));
    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage(_nonExtensionStageName));
}

TEST_F(IsRegisteredExtensionStageTest, ReturnsFalseForBuiltInStages) {
    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage("$match"_sd));
    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage("$project"_sd));
    ASSERT_FALSE(LiteParsedDocumentSource::isRegisteredExtensionStage("$limit"_sd));
}

}  // namespace mongo
