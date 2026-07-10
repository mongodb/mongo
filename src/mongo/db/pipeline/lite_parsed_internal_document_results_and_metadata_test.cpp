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

#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"

#include "mongo/bson/json.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class LiteParsedInternalDocumentResultsAndMetadataTest : public AggregationContextFixture {
protected:
    std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed> parse(const BSONObj& spec) {
        return InternalDocumentResultsAndMetadataLiteParsed::parse(
            getExpCtx()->getNamespaceString(), spec.firstElement(), {});
    }
};

using LiteParsedInternalDocumentResultsAndMetadataDeathTest =
    LiteParsedInternalDocumentResultsAndMetadataTest;

DEATH_TEST_F(LiteParsedInternalDocumentResultsAndMetadataDeathTest,
             RejectsNonObjectSpec,
             "FailedToParse") {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: "string"})");
    parse(spec);
}

DEATH_TEST_F(LiteParsedInternalDocumentResultsAndMetadataDeathTest,
             RejectsMissingSource,
             "FailedToParse") {
    auto spec =
        fromjson(R"({$_internalDocumentResultsAndMetadata: {metadata: {as: "SEARCH_META"}}})");
    parse(spec);
}

DEATH_TEST_F(LiteParsedInternalDocumentResultsAndMetadataDeathTest,
             RejectsNonObjectSource,
             "TypeMismatch") {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: {source: "string"}})");
    parse(spec);
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest, GetStageParamsReturnsRightType) {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$documents: []}}})");
    auto lp = parse(spec);
    auto params = lp->getStageParams();
    ASSERT_TRUE(dynamic_cast<InternalDocumentResultsAndMetadataStageParams*>(params.get()));
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       RequiredPrivilegesEmptyForDocumentsSource) {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$documents: []}}})");
    auto lp = parse(spec);
    ASSERT_EQ(lp->requiredPrivileges(false /*isMongos*/, false /*bypass*/).size(), 0u);
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       RequiredPrivilegesForwardsFromNestedSource) {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$collStats: {}}}})");
    auto lp = parse(spec);
    auto privs = lp->requiredPrivileges(false /*isMongos*/, false /*bypass*/);
    ASSERT_EQ(privs.size(), 1u);
    ASSERT_TRUE(privs[0].getActions().contains(ActionType::collStats));
    ASSERT_EQ(privs[0].getResourcePattern(),
              ResourcePattern::forExactNamespace(getExpCtx()->getNamespaceString()));
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       GetFirstStageViewApplicationPolicyReturnsDoNothing) {
    auto spec = fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$documents: []}}})");
    auto lp = parse(spec);
    ASSERT_EQ(lp->getFirstStageViewApplicationPolicy(),
              FirstStageViewApplicationPolicy::kDoNothing);
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       BindResolvedNamespaceSurvivesGetStageParams) {
    // The view binding applied at the LP layer must travel through getStageParams() so it reaches
    // DocumentSource construction; otherwise createFromStageParams would re-parse the inner stage
    // from view-unaware state and silently drop the view.
    auto spec = fromjson(
        R"({$_internalDocumentResultsAndMetadata: {source: {$_internalSearchIdLookup: {limit: 100}}}})");
    auto lp = parse(spec);

    const auto kViewNss =
        NamespaceString::createNamespaceString_forTest("unittests.view_for_metadata");
    const auto kResolvedNss =
        NamespaceString::createNamespaceString_forTest("unittests.resolved_for_metadata");
    std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("status" << "active"))};
    auto view = ResolvedNamespace::makeForView(kViewNss, kResolvedNss, viewPipeline);

    lp->bindResolvedNamespace(view, {});

    auto params = lp->getStageParams();
    auto* typedParams = dynamic_cast<InternalDocumentResultsAndMetadataStageParams*>(params.get());
    ASSERT_TRUE(typedParams != nullptr);

    const auto& sourceStages = typedParams->getSourcePipeline()->getStages();
    ASSERT_EQ(sourceStages.size(), 1u);
    auto* idLookup = dynamic_cast<LiteParsedInternalSearchIdLookUp*>(sourceStages[0].get());
    ASSERT_TRUE(idLookup != nullptr);
    ASSERT_TRUE(idLookup->getSpec().getViewPipeline().has_value());
    ASSERT_EQ(idLookup->getSpec().getViewPipeline()->size(), 1u);
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest, ForwardsRankedFromNestedSource) {
    // $sort is a ranked (but not scored) stage.
    auto lp =
        parse(fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$sort: {x: 1}}}})"));
    ASSERT_TRUE(lp->isRankedStage());
    ASSERT_FALSE(lp->isScoredStage());
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       ForwardsScoredAndSelectionFromNestedSource) {
    // $score is a scored + selection stage, but not ranked.
    auto lp = parse(
        fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$score: {score: 1.0}}}})"));
    ASSERT_TRUE(lp->isScoredStage());
    ASSERT_TRUE(lp->isSelectionStage());
    ASSERT_FALSE(lp->isRankedStage());
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       ForwardsScoreDetailsFromNestedSourceWhenRequested) {
    // $score reports scoreDetails only when the "scoreDetails" option is set.
    auto lp = parse(fromjson(
        R"({$_internalDocumentResultsAndMetadata: {source: {$score: {score: 1.0, scoreDetails: true}}}})"));
    ASSERT_TRUE(lp->isScoreDetailsStage());
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest,
       DoesNotForwardScoreDetailsWhenNestedSourceOmitsIt) {
    auto lp = parse(
        fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$score: {score: 1.0}}}})"));
    ASSERT_FALSE(lp->isScoreDetailsStage());
}

TEST_F(LiteParsedInternalDocumentResultsAndMetadataTest, ReportsNoHybridPropertiesForPlainSource) {
    // A source that is neither ranked, scored, scoreDetails, nor selection-only forwards all false.
    auto lp =
        parse(fromjson(R"({$_internalDocumentResultsAndMetadata: {source: {$collStats: {}}}})"));
    ASSERT_FALSE(lp->isRankedStage());
    ASSERT_FALSE(lp->isScoredStage());
    ASSERT_FALSE(lp->isScoreDetailsStage());
    ASSERT_FALSE(lp->isSelectionStage());
}

}  // namespace
}  // namespace mongo
