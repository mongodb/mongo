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
#include "mongo/db/pipeline/lite_parsed_internal_hybrid_search.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/test_lite_parsed.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test.liteParsedPipeline"sv);
const NamespaceString kViewNss = NamespaceString::createNamespaceString_forTest("test.view"sv);
const NamespaceString kResolvedNss =
    NamespaceString::createNamespaceString_forTest("test.collection"sv);

/**
 * Helper function to create a view ResolvedNamespace with a simple pipeline.
 */
ResolvedNamespace createTestView(std::vector<BSONObj> viewPipeline) {
    return ResolvedNamespace::makeForView(kViewNss, kResolvedNss, std::move(viewPipeline));
}

TEST(LiteParsedPipelineTest, HandleViewStitchesViewBeforeUserPipe) {
    std::vector<BSONObj> userStages = {BSON("$group" << BSON("_id" << "$field")),
                                       BSON("$sort" << BSON("_id" << 1))};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 10)};
    const auto view = createTestView(std::move(viewStages));

    pipeline.handleView(view, {});

    // Verify the pipeline now contains the view stages in the correct order.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 4U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$group");
    ASSERT_EQ(stages[3]->getParseTimeName(), "$sort");
}

DEATH_TEST_REGEX(LiteParsedInternalHybridSearchDeathTest,
                 ViewApplicationPolicyIsUnreachable,
                 "Tripwire assertion.*12109100") {
    // The desugarer (SERVER-121970/121974) only ever appends the marker, never at position 0,
    // so view application must never consult its policy.
    LiteParsedInternalHybridSearch stage;
    try {
        stage.getFirstStageViewApplicationPolicy();
    } catch (const DBException&) {
        // Expected: the tassert throws and trips the tripwire; the process aborts at exit.
    }
}

TEST(LiteParsedPipelineTest, HandleViewEmptyUserPipelineBecomesViewPipeline) {
    // Create an empty user pipeline.
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{});

    // Create a view with two stages.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)),
                                       BSON("$project" << BSON("y" << 1))};
    const auto view = createTestView(std::move(viewStages));

    pipeline.handleView(view, {});

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
    const auto view = createTestView({});

    pipeline.handleView(view, {});

    // User pipeline should be the same.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 1U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
}

TEST(LiteParsedPipelineTest, HandleViewDoesNotConsumeOrMutateView) {
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{});

    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1))};
    const auto view = createTestView(std::move(viewStages));

    ASSERT_FALSE(view.getOriginalBson().empty());
    const auto before = view.getOriginalBson();
    ASSERT_EQ(before.size(), 1U);

    pipeline.handleView(view, {});

    ASSERT_FALSE(view.getOriginalBson().empty());
    const auto after = view.getOriginalBson();
    ASSERT_EQ(after.size(), 1U);
    ASSERT_BSONOBJ_EQ(before[0], after[0]);
}

TEST(LiteParsedPipelineTest, HandleViewStitchedPipelineSurvivesViewLifetime) {
    // User pipeline has one stage so we can see ordering.
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("x" << 1))});
    {
        // The view ResolvedNamespace lives only in this scope.
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 5)};
        auto view = createTestView(std::move(viewStages));
        pipeline.handleView(view, {});
    }
    // View pipeline stages own their backing BSON, so they remain valid after the view entry is
    // destroyed. Verify the pipeline now contains the view stages in the correct order.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 3U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$limit");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$sort");
}

TEST(LiteParsedPipelineTest, ViewCloneIsIndependentOfOriginalLifetime) {
    ResolvedNamespace cloned;
    {
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)),
                                           BSON("$project" << BSON("y" << 1))};
        auto view = createTestView(std::move(viewStages));
        cloned = view.clone();
    }
    ASSERT_FALSE(cloned.getOriginalBson().empty());
    LiteParsedPipeline userPipe(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("y" << 1))});

    userPipe.handleView(cloned, {});

    const auto& stages = userPipe.getStages();
    ASSERT_EQ(stages.size(), 3U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$project");
    ASSERT_EQ(stages[2]->getParseTimeName(), "$sort");
}

TEST(LiteParsedPipelineTest, GetSerializedViewPipelineReturnsEquivalentBsonWhenNotDesugared) {
    // Build a view ResolvedNamespace with a two-stage pipeline that has NOT been desugared.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)),
                                       BSON("$project" << BSON("y" << 1))};
    const auto view = createTestView(viewStages);

    auto serialized = view.getSerializedViewPipeline();
    ASSERT_EQ(serialized.size(), viewStages.size());
    for (size_t i = 0; i < viewStages.size(); ++i) {
        ASSERT_BSONOBJ_EQ(serialized[i], viewStages[i]);
    }

    // And it should also match getOriginalBson() exactly.
    auto original = view.getOriginalBson();
    ASSERT_EQ(original.size(), serialized.size());
    for (size_t i = 0; i < original.size(); ++i) {
        ASSERT_BSONOBJ_EQ(original[i], serialized[i]);
    }
}

TEST(LiteParsedPipelineTest,
     GetSerializedViewPipelineReflectsModifiedStagesWhileOriginalBsonIsPreserved) {
    // Build a view ResolvedNamespace with a single-stage pipeline.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1))};
    auto view = createTestView(viewStages);

    BSONObj newStage1 = BSON("$match" << BSON("a" << 2));
    BSONObj newStage2 = BSON("$limit" << 5);

    std::vector<std::unique_ptr<LiteParsedDocumentSource>> replacements;
    replacements.push_back(LiteParsedDocumentSource::parse(kResolvedNss, newStage1));
    replacements.push_back(LiteParsedDocumentSource::parse(kResolvedNss, newStage2));
    for (auto& stage : replacements) {
        stage->makeOwned();
    }

    // getViewPipeline() returns a copy. Modify the copy, not the view entry's internal pipeline.
    auto modifiedPipeline = view.getViewPipeline();
    modifiedPipeline.replaceStageWith(0, std::move(replacements));

    // getOriginalBson() and getSerializedViewPipeline() on the original view entry should still
    // return the original, unmodified pipeline since we only mutated the copy.
    auto original = view.getOriginalBson();
    ASSERT_EQ(original.size(), 1U);
    ASSERT_BSONOBJ_EQ(original[0], viewStages[0]);

    auto serialized = view.getSerializedViewPipeline();
    ASSERT_EQ(serialized.size(), 1U);
    ASSERT_BSONOBJ_EQ(serialized[0], viewStages[0]);
}

TEST(LiteParsedPipelineTest, ClonedPipelineWithViewStagesPreservesOwnership) {
    // Create a pipeline with view stages stitched in.
    LiteParsedPipeline original(kTestNss, std::vector<BSONObj>{BSON("$sort" << BSON("x" << 1))});
    {
        std::vector<BSONObj> viewStages = {BSON("$match" << BSON("x" << 1)), BSON("$limit" << 5)};
        auto view = createTestView(std::move(viewStages));
        original.handleView(view, {});
    }

    // Clone the pipeline after the view entry is destroyed. The cloned stages should also own
    // their BSON.
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

TEST(LiteParsedPipelineTest, ReplaceStageWithOwnsUnownedClonedSubpipelineStages) {
    // The replacements are unowned clones of the $unionWith's sub-pipeline stages, which are
    // backed by the stage's OwnedLiteParsedPipeline buffers (the hybrid-search desugar shape:
    // the first input pipeline's stages are spliced inline as clones). Erasing the stage frees
    // that backing, so without the makeOwned() pass in replaceStageWith() the replacements
    // dangle — a use-after-free caught under ASAN.
    BSONObj unionSpec =
        BSON("$unionWith" << BSON(
                 "coll" << "other"
                        << "pipeline"
                        << BSON_ARRAY(BSON("$match" << BSON("x" << 1)) << BSON("$limit" << 5))));
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{unionSpec});

    // Unowned clones backed by the $unionWith stage's OwnedLiteParsedPipeline.
    const auto* subPipelines = pipeline.getStages()[0]->getSubPipelines();
    ASSERT(subPipelines);
    ASSERT_EQ(subPipelines->size(), 1U);
    std::vector<std::unique_ptr<LiteParsedDocumentSource>> replacements;
    for (const auto& subStage : (*subPipelines)[0]->getStages()) {
        replacements.push_back(subStage->clone());
    }

    // Erases the sole owner of the replacements' backing BSON.
    pipeline.replaceStageWith(0, std::move(replacements));

    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 2U);
    ASSERT_EQ(stages[0]->getParseTimeName(), "$match");
    ASSERT_EQ(stages[1]->getParseTimeName(), "$limit");
    ASSERT_BSONOBJ_EQ(stages[0]->getOriginalBson().wrap(), BSON("$match" << BSON("x" << 1)));
    ASSERT_BSONOBJ_EQ(stages[1]->getOriginalBson().wrap(), BSON("$limit" << 5));
}

TEST(LiteParsedInternalHybridSearchTest, ParseRejectsInvalidSpecs) {
    // The lite parser must validate the spec itself: the StageParams registry hands
    // createFromBson this stage's _originalBson, so a parse() that discarded the user's spec
    // would silently normalize garbage to {}.
    ASSERT_THROWS_CODE(LiteParsedInternalHybridSearch::parse(
                           kTestNss, BSON("$_internalHybridSearch" << 1).firstElement(), {}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(
        LiteParsedInternalHybridSearch::parse(
            kTestNss, BSON("$_internalHybridSearch" << BSON("unexpected" << 1)).firstElement(), {}),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST(LiteParsedPipelineTest, ReplaceStageWithNestedPipelineReplacementOwnsSubStages) {
    // A replacement stage that itself holds sub-pipelines: the sub-stages must stay valid after
    // the erased stage's buffer dies. OwnedLiteParsedPipeline guarantees this by giving every
    // sub-pipeline stage self-owned BSON at construction/copy.
    LiteParsedPipeline pipeline = [&] {
        BSONObj unionSpec =
            BSON("$unionWith" << BSON("coll" << "other"
                                             << "pipeline"
                                             << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))));
        LiteParsedPipeline parsed(kTestNss, std::vector<BSONObj>{unionSpec});
        auto cloned = parsed.clone();
        // clone() copies parse state but does not take ownership of unowned stages; own them
        // explicitly so the pipeline outlives 'unionSpec'.
        cloned.makeOwned();
        return cloned;
    }();

    // Clone the whole $unionWith (a nested-pipelines stage) as the replacement.
    std::vector<std::unique_ptr<LiteParsedDocumentSource>> replacements;
    replacements.push_back(pipeline.getStages()[0]->clone());

    pipeline.replaceStageWith(0, std::move(replacements));

    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 1U);
    const auto* subPipelines = stages[0]->getSubPipelines();
    ASSERT(subPipelines);
    ASSERT_EQ(subPipelines->size(), 1U);
    const auto& subStages = (*subPipelines)[0]->getStages();
    ASSERT_EQ(subStages.size(), 1U);
    ASSERT_BSONOBJ_EQ(subStages[0]->getOriginalBson().wrap(), BSON("$match" << BSON("x" << 1)));
}

TEST(LiteParsedPipelineTest, GetParseNssMatchesConstructorNss) {
    std::vector<BSONObj> stages = {BSON("$match" << BSON("x" << 1))};
    LiteParsedPipeline pipeline(kTestNss, stages);
    ASSERT_EQ(pipeline.getOriginalParseNss(), kTestNss);
}

TEST(LiteParsedPipelineTest, NestedLookupSubpipelineGetParseNssIsForeignCollection) {
    std::vector<BSONObj> pipelineStages = {
        BSON("$lookup" << BSON("from" << "otherCollection"
                                      << "let" << BSONObj() << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("a" << 1))) << "as"
                                      << "joined")),
    };
    LiteParsedPipeline pipeline(kTestNss, pipelineStages);
    const NamespaceString kForeignNss =
        NamespaceString::createNamespaceString_forTest(kTestNss.dbName(), "otherCollection");

    ASSERT_EQ(pipeline.getOriginalParseNss(), kTestNss);

    auto* subPipelinesPtr = pipeline.getStages()[0]->getSubPipelines();
    ASSERT_NE(subPipelinesPtr, nullptr);
    const auto& subPipelines = *subPipelinesPtr;
    ASSERT_EQ(subPipelines.size(), 1U);
    ASSERT_EQ(subPipelines[0]->getOriginalParseNss(), kForeignNss);
}

TEST(LiteParsedPipelineTest, NestedMergeSubpipelineGetParseNssIsTargetCollection) {
    // The whenMatched update pipeline runs against the target collection, so its parse nss should
    // be the target namespace - matching the convention $lookup/$unionWith already follow.
    std::vector<BSONObj> pipelineStages = {
        BSON("$merge" << BSON("into" << "targetCollection"
                                     << "whenMatched"
                                     << BSON_ARRAY(BSON("$addFields" << BSON("x" << 1))))),
    };
    LiteParsedPipeline pipeline(kTestNss, pipelineStages);
    const NamespaceString kTargetNss =
        NamespaceString::createNamespaceString_forTest(kTestNss.dbName(), "targetCollection");

    ASSERT_EQ(pipeline.getOriginalParseNss(), kTestNss);

    const auto* subPipelines = pipeline.getStages()[0]->getSubPipelines();
    ASSERT_NE(subPipelines, nullptr);
    ASSERT_EQ(subPipelines->size(), 1U);
    ASSERT_EQ((*subPipelines)[0]->getOriginalParseNss(), kTargetNss);
}

TEST(LiteParsedPipelineTest, HandleViewPreservesParseNss) {
    std::vector<BSONObj> userStages = {BSON("$match" << BSON("x" << 1))};
    LiteParsedPipeline pipeline(kTestNss, userStages);
    pipeline.handleView(createTestView({BSON("$limit" << 1)}), {});
    ASSERT_EQ(pipeline.getOriginalParseNss(), kTestNss);
}

}  // namespace

std::unique_ptr<LiteParsedDocumentSource> createViewPolicyDefaultParser(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    return std::make_unique<TestLiteParsed>(spec, FirstStageViewApplicationPolicy::kDefaultPrepend);
}

std::unique_ptr<LiteParsedDocumentSource> createViewPolicyDoNothingParser(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    return std::make_unique<TestLiteParsed>(spec, FirstStageViewApplicationPolicy::kDoNothing);
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

    /** Unregisters a parser by name. Used by tests that register additional parsers. */
    static void unregisterParserByName(const std::string& name) {
        LiteParsedDocumentSource::unregisterParser_forTest(name);
    }

    BSONObj defaultStageSpec() const {
        return BSON(_defaultStageName << BSONObj());
    }

    BSONObj doNothingStageSpec() const {
        return BSON(_doNothingStageName << BSONObj());
    }

    ResolvedNamespace makeView() const {
        return createTestView({BSON("$match" << BSON("x" << 1))});
    }

    std::string _defaultStageName;
    std::string _doNothingStageName;
};

TEST_F(LiteParsedPipelineViewPolicyTest, FirstDoNothingSuppressesPrepend) {
    std::vector<BSONObj> userStages = {doNothingStageSpec(), defaultStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto view = makeView();
    pipeline.handleView(view, {});

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 2U);
    ASSERT_EQ(out[0]->getParseTimeName(), _doNothingStageName);
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
}

TEST_F(LiteParsedPipelineViewPolicyTest,
       LaterDoNothingDoesNotAffectPrependIfFirstIsDefaultPrepend) {
    std::vector<BSONObj> userStages = {defaultStageSpec(), doNothingStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto view = makeView();
    pipeline.handleView(view, {});

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 3U);
    ASSERT_EQ(out[0]->getParseTimeName(), "$match");
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
    ASSERT_EQ(out[2]->getParseTimeName(), _doNothingStageName);
}

TEST_F(LiteParsedPipelineViewPolicyTest, PrependWhenDefaultPrependIsTrueForAllStages) {
    std::vector<BSONObj> userStages = {defaultStageSpec(), defaultStageSpec()};
    LiteParsedPipeline pipeline(kTestNss, userStages);

    auto view = makeView();
    pipeline.handleView(view, {});

    const auto& out = pipeline.getStages();
    ASSERT_EQ(out.size(), 3U);
    ASSERT_EQ(out[0]->getParseTimeName(), "$match");
    ASSERT_EQ(out[1]->getParseTimeName(), _defaultStageName);
    ASSERT_EQ(out[2]->getParseTimeName(), _defaultStageName);
}

}  // namespace mongo
