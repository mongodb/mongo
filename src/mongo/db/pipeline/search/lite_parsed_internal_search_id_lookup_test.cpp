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
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("unittests.test");
const NamespaceString kViewNss =
    NamespaceString::createNamespaceString_forTest("unittests.view_test");
const NamespaceString kResolvedNss =
    NamespaceString::createNamespaceString_forTest("unittests.resolved_coll");

/**
 * Tests for LiteParsed::getViewPolicy() and LiteParsed::getStageParams() to verify the
 * ViewPolicy callback correctly stores view pipeline BSON for use in desugaring.
 */
class LiteParsedInternalSearchIdLookUpTest : public unittest::Test {};

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetViewPolicyReturnsDoNothingPolicy) {
    BSONObj spec = BSON("$_internalSearchIdLookup" << BSON("limit" << 100));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    auto viewPolicy = liteParsed->getViewPolicy();

    // The policy should be kDoNothing since IdLookup handles view resolution itself.
    ASSERT_EQ(viewPolicy.policy, ViewPolicy::kFirstStageApplicationPolicy::kDoNothing);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, ViewPolicyCallbackStoresViewPipelineBson) {
    BSONObj spec = BSON("$_internalSearchIdLookup" << BSON("limit" << 100));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Create a view pipeline with a $match and $project stage.
    std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("status" << "active")),
                                         BSON("$project" << BSON("name" << 1 << "status" << 1))};
    ViewInfo viewInfo(kViewNss, kResolvedNss, viewPipeline);

    // Invoke the callback.
    auto viewPolicy = liteParsed->getViewPolicy();
    viewPolicy.callback(viewInfo, "$_internalSearchIdLookup");

    // Now getStageParams should return params with the view pipeline BSON.
    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Verify the view pipeline was captured correctly.
    ASSERT_TRUE(typedParams->viewPipeline);

    const auto& stages = typedParams->viewPipeline->getStages();
    ASSERT_EQ(stages.size(), 2U);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetStageParamsReturnsLimitFromSpec) {
    BSONObj spec = BSON("$_internalSearchIdLookup" << BSON("limit" << 42));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Verify the limit was extracted correctly from the spec.
    ASSERT_EQ(typedParams->limit, 42);
    // Without view callback, the view pipeline should be empty.
    ASSERT_FALSE(typedParams->viewPipeline);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, GetStageParamsReturnsZeroLimitWhenNotSpecified) {
    BSONObj spec = BSON("$_internalSearchIdLookup" << BSONObj());
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // When limit is not specified, it should be 0.
    ASSERT_EQ(typedParams->limit, 0);
    ASSERT_FALSE(typedParams->viewPipeline);
}

TEST_F(LiteParsedInternalSearchIdLookUpTest, ViewPolicyCallbackWithEmptyViewPipeline) {
    BSONObj spec = BSON("$_internalSearchIdLookup" << BSON("limit" << 10));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Create an empty view pipeline.
    ViewInfo viewInfo(kViewNss, kResolvedNss, {});

    auto viewPolicy = liteParsed->getViewPolicy();
    viewPolicy.callback(viewInfo, "$_internalSearchIdLookup");

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<InternalSearchIdLookupStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);

    // Empty view pipeline should result in an empty stored pipeline.
    ASSERT_TRUE(typedParams->viewPipeline);

    const auto& stages = typedParams->viewPipeline->getStages();
    ASSERT_EQ(stages.size(), 0);
    ASSERT_EQ(typedParams->limit, 10);
}

}  // namespace
}  // namespace mongo
