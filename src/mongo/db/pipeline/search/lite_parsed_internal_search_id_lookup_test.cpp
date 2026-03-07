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

#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/views/pipeline_resolver.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("unittests.test");
const NamespaceString kViewNss =
    NamespaceString::createNamespaceString_forTest("unittests.view_test");
const NamespaceString kResolvedNss =
    NamespaceString::createNamespaceString_forTest("unittests.resolved_coll");

/**
 * Tests for LiteParsed::bindViewInfo() and LiteParsed::getStageParams() to verify that
 * bindViewInfo correctly stores view pipeline BSON for use in desugaring.
 */
class LiteParsedInternalSearchIdLookUpTest : public unittest::Test {};

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetFirstStageViewApplicationPolicyReturnsDoNothing) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 100LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // The policy should be kDoNothing since IdLookup handles view resolution itself.
    ASSERT_EQ(liteParsed->getFirstStageViewApplicationPolicy(),
              FirstStageViewApplicationPolicy::kDoNothing);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, BindViewInfoStoresViewPipelineBson) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 100LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Create a view pipeline with a $match and $project stage.
    std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("status" << "active")),
                                         BSON("$project" << BSON("name" << 1 << "status" << 1))};
    ViewInfo viewInfo(kViewNss, kResolvedNss, viewPipeline);

    liteParsed->bindViewInfo(viewInfo, {});

    // Now getStageParams should return params with the view pipeline BSON.
    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Verify the view pipeline was captured correctly.
    ASSERT(typedParams->ownedSpec.getViewPipeline());

    const auto& stages = typedParams->ownedSpec.getViewPipeline().get();
    ASSERT_EQ(stages.size(), 2U);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetStageParamsReturnsLimitFromSpec) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 42LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Verify the limit was extracted correctly from the spec.
    ASSERT(typedParams->ownedSpec.getLimit());
    ASSERT_EQ(typedParams->ownedSpec.getLimit().get(), 42);
    // Without bindViewInfo call, the view pipeline should be empty.
    ASSERT_FALSE(typedParams->ownedSpec.getViewPipeline());
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetStageParamsReturnsNothingWhenNotSpecified) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSONObj());
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    ASSERT_FALSE(typedParams->ownedSpec.getLimit());
    ASSERT_FALSE(typedParams->ownedSpec.getViewPipeline());
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, BindViewInfoWithEmptyViewPipeline) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 10LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Create an empty view pipeline.
    ViewInfo viewInfo(kViewNss, kResolvedNss, {});

    liteParsed->bindViewInfo(viewInfo, {});

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Empty view pipeline should result in an empty stored pipeline.
    ASSERT(typedParams->ownedSpec.getViewPipeline());

    const auto& stages = typedParams->ownedSpec.getViewPipeline().get();
    ASSERT_EQ(stages.size(), 0);

    ASSERT(typedParams->ownedSpec.getLimit());
    ASSERT_EQ(typedParams->ownedSpec.getLimit().get(), 10);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, BsonSpecSurvivesAfterOriginalDestroyed) {
    std::unique_ptr<LiteParsedInternalSearchIdLookUp> liteParsed;
    const long long expectedLimit = 123;

    {
        // Create BSONObj in a limited scope.
        BSONObj spec =
            BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << expectedLimit));
        liteParsed = LiteParsedInternalSearchIdLookUp::parse(
            kTestNss, spec.firstElement(), LiteParserOptions{});
        ASSERT_EQ(liteParsed->getSpec().getLimit().get(), expectedLimit);
    }
    // Original BSONObj is now destroyed.

    // Verify getSpec() still returns valid data after original is destroyed.
    const auto& ownedSpec = liteParsed->getSpec();
    ASSERT_TRUE(ownedSpec.getLimit());
    ASSERT_EQ(ownedSpec.getLimit().get(), expectedLimit);

    // Verify getOriginalBson() still returns valid data after original is destroyed.
    ASSERT_FALSE(liteParsed->getOriginalBson().eoo());

    // Verify getStageParams() still works correctly.
    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);
    ASSERT_EQ(typedParams->ownedSpec.getLimit().get(), expectedLimit);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetSpecReturnsConsistentReference) {
    BSONObj spec = BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 456LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Get references to the owned spec.
    const auto& ownedSpec1 = liteParsed->getSpec();
    const auto& ownedSpec2 = liteParsed->getSpec();

    // Both references should point to the same object.
    ASSERT_EQ(&ownedSpec1, &ownedSpec2);
    ASSERT_EQ(ownedSpec1.getLimit().get(), 456);

    // Verify getOriginalBson() returns consistent data across calls.
    auto bson1 = liteParsed->getOriginalBson();
    auto bson2 = liteParsed->getOriginalBson();
    ASSERT_TRUE(bson1.binaryEqualValues(bson2));
}

TEST_F(LiteParsedInternalSearchIdLookUpTest,
       ApplyViewToLiteParsedStoresDesugaredViewPipelineInIdLookup) {
    // Build a user pipeline consisting of a single $_internalSearchIdLookup stage.
    BSONObj idLookupSpec =
        BSON(LiteParsedInternalSearchIdLookUp::kStageName << BSON("limit" << 100LL));
    LiteParsedPipeline pipeline(kTestNss, std::vector<BSONObj>{idLookupSpec});

    // Create a ResolvedView with a two-stage view pipeline.
    std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("status" << "active")),
                                         BSON("$project" << BSON("name" << 1 << "status" << 1))};
    const ResolvedView resolvedView{kResolvedNss, viewPipeline, BSONObj()};

    // Call applyViewToLiteParsed() which desugars the view pipeline and invokes handleView().
    PipelineResolver::applyViewToLiteParsed(
        &pipeline, resolvedView, kViewNss, ResolvedNamespaceMap{});

    // IdLookup has a kDoNothing policy so the view pipeline should NOT be prepended.
    const auto& stages = pipeline.getStages();
    ASSERT_EQ(stages.size(), 1U);
    ASSERT_EQ(stages[0]->getParseTimeName(), LiteParsedInternalSearchIdLookUp::kStageName);

    // The IdLookup stage should now carry the desugared view pipeline via bindViewInfo().
    auto* idLookup = dynamic_cast<LiteParsedInternalSearchIdLookUp*>(stages[0].get());
    ASSERT_TRUE(idLookup != nullptr);

    const auto& spec = idLookup->getSpec();
    ASSERT_TRUE(spec.getViewPipeline());

    const auto& storedPipeline = spec.getViewPipeline().get();
    ASSERT_EQ(storedPipeline.size(), 2U);
    ASSERT_BSONOBJ_EQ(storedPipeline[0], viewPipeline[0]);
    ASSERT_BSONOBJ_EQ(storedPipeline[1], viewPipeline[1]);
}

}  // namespace
}  // namespace mongo
