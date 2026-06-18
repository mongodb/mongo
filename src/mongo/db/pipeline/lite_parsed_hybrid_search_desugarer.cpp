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

#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer.h"

#include "mongo/base/init.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer_utils.h"
#include "mongo/db/pipeline/lite_parsed_internal_hybrid_search.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion_desugarer_utils.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion_desugarer_utils.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/util/rank_fusion_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <utility>

namespace mongo::lite_parsed_hybrid_search_desugarer {
using namespace std::literals::string_view_literals;

namespace {

// Pairs each input pipeline name with its parsed LiteParsedPipeline, sorted by name so
// ordering-sensitive output (e.g. scoreDetails) is deterministic.
std::vector<std::pair<std::string, const OwnedLiteParsedPipeline*>> collectSortedInputPipelines(
    const BSONObj& pipelinesSpec, const std::vector<OwnedLiteParsedPipeline>& subPipelines) {
    std::vector<std::pair<std::string, const OwnedLiteParsedPipeline*>> sorted;
    sorted.reserve(subPipelines.size());
    {
        size_t idx = 0;
        for (const auto& elem : pipelinesSpec) {
            sorted.emplace_back(elem.fieldName(), &subPipelines[idx++]);
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return sorted;
}

std::vector<std::string> namesOf(
    const std::vector<std::pair<std::string, const OwnedLiteParsedPipeline*>>& sorted) {
    std::vector<std::string> names;
    names.reserve(sorted.size());
    for (const auto& [name, _] : sorted) {
        names.push_back(name);
    }
    return names;
}

// Splices the first input pipeline's stages directly into 'out'; wraps each subsequent one in a
// $unionWith ending with the $_internalHybridSearch marker. The marker goes LAST: the timeseries
// guard is position-independent, and the sub-pipeline's real first stage stays up front for
// mongot detection and view stitching.
void appendInputPipeline(StageSpecs perPipeline,
                         bool isFirst,
                         const NamespaceString& nss,
                         StringData userCollName,
                         StageSpecs& out) {
    if (isFirst) {
        for (auto& s : perPipeline) {
            out.push_back(std::move(s));
        }
        return;
    }
    perPipeline.push_back(std::make_unique<LiteParsedInternalHybridSearch>());
    out.push_back(common_utils::buildUnionWithLPDS(nss, userCollName, std::move(perPipeline)));
}

}  // namespace

StageSpecs desugarRankFusion(const LiteParsedRankFusion& stage,
                             const NamespaceString& nss,
                             std::string_view userCollName) {
    const auto& spec = stage.getSpec();
    const auto& subPipelines = *stage.getSubPipelines();
    const bool includeScoreDetails = spec.getScoreDetails();

    auto sorted = collectSortedInputPipelines(spec.getInput().getPipelines(), subPipelines);
    auto pipelineNames = namesOf(sorted);

    tassert(12559411,
            "$rankFusion: subPipelines and pipeline-name list size mismatch",
            pipelineNames.size() == subPipelines.size());

    // Resolve and validate weights against the input pipeline names; otherwise empty (all default
    // to 1).
    StringMap<double> weights;
    if (const auto& combinationSpec = spec.getCombination()) {
        weights = common_utils::validateWeights(
            combinationSpec->getWeights(), pipelineNames, "$rankFusion"sv);
    }

    StageSpecs out;

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& pipelineName = sorted[i].first;
        const LiteParsedPipeline& subPipeline = sorted[i].second->pipeline();
        double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);

        StageSpecs perPipeline = rank_fusion_utils::buildRankFusionInputPipelinePreamble(
            nss, subPipeline, pipelineName, weight, includeScoreDetails);

        appendInputPipeline(std::move(perPipeline), i == 0, nss, userCollName, out);
    }

    // ---- Tail: group + replaceRoot + rank-NA mutation + score/sort/project (+ scoreDetails) ----
    out.push_back(common_utils::parseOwnedStage(
        nss,
        common_utils::buildGroupBson(pipelineNames,
                                     includeScoreDetails,
                                     rank_fusion_utils::kInternalFieldsName,
                                     rank_fusion_utils::kDocsName,
                                     rank_fusion_utils::kDetailsScalarSuffix)));
    out.push_back(common_utils::parseOwnedStage(
        nss,
        common_utils::buildReplaceRootMergeBson(pipelineNames,
                                                includeScoreDetails,
                                                rank_fusion_utils::kInternalFieldsName,
                                                rank_fusion_utils::kDocsName,
                                                rank_fusion_utils::kDetailsScalarSuffix)));
    out.push_back(common_utils::parseOwnedStage(
        nss, rank_fusion_utils::buildRankAddFieldsBson(pipelineNames)));

    // TODO SERVER-85426: Remove this branch once all feature flags have been removed.
    if (isRankFusionFullEnabled()) {
        out.push_back(common_utils::parseOwnedStage(
            nss, rank_fusion_utils::buildSetMetadataScoreBson(pipelineNames)));
        if (includeScoreDetails) {
            out.push_back(common_utils::parseOwnedStage(
                nss,
                rank_fusion_utils::buildCalculatedFinalScoreDetailsBson(pipelineNames, weights)));
            out.push_back(common_utils::parseOwnedStage(
                nss, rank_fusion_utils::buildSetMetadataScoreDetailsBson()));
        }
        out.push_back(common_utils::parseOwnedStage(nss, common_utils::buildSortByScoreMetaBson()));
    } else {
        out.push_back(common_utils::parseOwnedStage(
            nss, rank_fusion_utils::buildAddFieldsScoreBson(pipelineNames)));
        out.push_back(
            common_utils::parseOwnedStage(nss, rank_fusion_utils::buildSortByScoreScalarBson()));
    }
    out.push_back(common_utils::parseOwnedStage(nss,
                                                common_utils::buildProjectRemoveInternalFieldsBson(
                                                    rank_fusion_utils::kInternalFieldsName)));

    // Append the marker last: the constraint walk is position-independent and the natural first
    // stage stays up front for view handling and mongot detection.
    out.push_back(std::make_unique<LiteParsedInternalHybridSearch>());

    return out;
}

size_t rankFusionStageExpander(LiteParsedPipeline* pipeline,
                               size_t index,
                               LiteParsedDocumentSource& stage) {
    auto* rankFusionStage = dynamic_cast<LiteParsedRankFusion*>(&stage);
    tassert(12559412,
            "rankFusionStageExpander invoked with non-$rankFusion stage",
            rankFusionStage != nullptr);
    if (!rankFusionStage->extensionsInHybridSearchEnabled()) {
        // With the flag off, $rankFusion desugars at DocumentSource parse time instead; advance
        // past it.
        return index + 1;
    }
    const NamespaceString& nss = pipeline->getOriginalParseNss();
    std::string_view userCollName = nss.coll();
    auto desugared = desugarRankFusion(*rankFusionStage, nss, userCollName);
    return pipeline->replaceStageWith(index, std::move(desugared));
}

StageSpecs desugarScoreFusion(const LiteParsedScoreFusion& stage,
                              const NamespaceString& nss,
                              std::string_view userCollName) {
    const auto& spec = stage.getSpec();
    const auto& subPipelines = *stage.getSubPipelines();
    const bool includeScoreDetails = spec.getScoreDetails();

    auto sorted = collectSortedInputPipelines(spec.getInput().getPipelines(), subPipelines);
    auto pipelineNames = namesOf(sorted);

    tassert(12559413,
            "$scoreFusion: subPipelines and pipeline-name list size mismatch",
            pipelineNames.size() == subPipelines.size());

    // Validate normalization / combination shape.
    score_fusion_utils::ScoreFusionScoringOptions scoringOptions(spec);

    StringMap<double> weights;
    if (const auto& combinationSpec = spec.getCombination()) {
        if (const auto& weightsObj = combinationSpec->getWeights()) {
            weights = common_utils::validateWeights(*weightsObj, pipelineNames, "$scoreFusion"sv);
        }
    }

    StageSpecs out;

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& pipelineName = sorted[i].first;
        const LiteParsedPipeline& subPipeline = sorted[i].second->pipeline();
        double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);

        StageSpecs perPipeline = score_fusion_utils::buildScoreFusionInputPipelinePreamble(
            nss,
            subPipeline,
            pipelineName,
            weight,
            scoringOptions.getNormalizationMethod(),
            includeScoreDetails);

        appendInputPipeline(std::move(perPipeline), i == 0, nss, userCollName, out);
    }

    // Tail: $group + $replaceRoot + $setMetadata score (+ optional scoreDetails) + $sort +
    // $project.
    out.push_back(common_utils::parseOwnedStage(
        nss,
        common_utils::buildGroupBson(pipelineNames,
                                     includeScoreDetails,
                                     score_fusion_utils::kInternalFieldsName,
                                     score_fusion_utils::kDocsName,
                                     score_fusion_utils::kDetailsScalarSuffix)));
    out.push_back(common_utils::parseOwnedStage(
        nss,
        common_utils::buildReplaceRootMergeBson(pipelineNames,
                                                includeScoreDetails,
                                                score_fusion_utils::kInternalFieldsName,
                                                score_fusion_utils::kDocsName,
                                                score_fusion_utils::kDetailsScalarSuffix)));
    out.push_back(common_utils::parseOwnedStage(
        nss, score_fusion_utils::buildSetFinalCombinedScoreBson(pipelineNames, scoringOptions)));

    if (includeScoreDetails) {
        out.push_back(common_utils::parseOwnedStage(
            nss, score_fusion_utils::buildCalculatedFinalScoreDetailsBson(pipelineNames, weights)));
        out.push_back(common_utils::parseOwnedStage(
            nss, score_fusion_utils::buildSetMetadataScoreDetailsBson(scoringOptions)));
    }

    out.push_back(common_utils::parseOwnedStage(nss, common_utils::buildSortByScoreMetaBson()));
    out.push_back(common_utils::parseOwnedStage(nss,
                                                common_utils::buildProjectRemoveInternalFieldsBson(
                                                    score_fusion_utils::kInternalFieldsName)));

    // Append the bookkeeping stage at the end. See desugarRankFusion for rationale.
    out.push_back(std::make_unique<LiteParsedInternalHybridSearch>());

    return out;
}

size_t scoreFusionStageExpander(LiteParsedPipeline* pipeline,
                                size_t index,
                                LiteParsedDocumentSource& stage) {
    auto* scoreFusionStage = dynamic_cast<LiteParsedScoreFusion*>(&stage);
    tassert(12559414,
            "scoreFusionStageExpander invoked with non-$scoreFusion stage",
            scoreFusionStage != nullptr);
    if (!scoreFusionStage->extensionsInHybridSearchEnabled()) {
        // With the flag off, $scoreFusion desugars at DocumentSource parse time instead; advance
        // past it.
        return index + 1;
    }
    const NamespaceString& nss = pipeline->getOriginalParseNss();
    std::string_view userCollName = nss.coll();
    auto desugared = desugarScoreFusion(*scoreFusionStage, nss, userCollName);
    return pipeline->replaceStageWith(index, std::move(desugared));
}

}  // namespace mongo::lite_parsed_hybrid_search_desugarer

namespace mongo {

MONGO_INITIALIZER_WITH_PREREQUISITES(RegisterRankFusionStageExpander, ("EndStageIdAllocation"))
(InitializerContext*) {
    tassert(12197000,
            "RankFusionStageParams::id not allocated",
            RankFusionStageParams::id != StageParams::kUnallocatedId);
    LiteParsedDesugarer::registerStageExpander(
        RankFusionStageParams::id, lite_parsed_hybrid_search_desugarer::rankFusionStageExpander);
}

MONGO_INITIALIZER_WITH_PREREQUISITES(RegisterScoreFusionStageExpander, ("EndStageIdAllocation"))
(InitializerContext*) {
    tassert(12197400,
            "ScoreFusionStageParams::id not allocated",
            ScoreFusionStageParams::id != StageParams::kUnallocatedId);
    LiteParsedDesugarer::registerStageExpander(
        ScoreFusionStageParams::id, lite_parsed_hybrid_search_desugarer::scoreFusionStageExpander);
}

}  // namespace mongo
