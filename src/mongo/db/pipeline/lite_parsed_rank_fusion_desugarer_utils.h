// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/rank_fusion_pipeline_builder.h"
#include "mongo/util/string_map.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::lite_parsed_hybrid_search_desugarer::rank_fusion_utils {
using namespace std::literals::string_view_literals;

inline constexpr int kRankConstant = RankFusionPipelineBuilder::kRankConstant;
inline constexpr std::string_view kInternalFieldsName =
    RankFusionPipelineBuilder::kRankFusionInternalFieldsName;
inline constexpr std::string_view kDocsName = RankFusionPipelineBuilder::kRankFusionDocsFieldName;
inline constexpr std::string_view kScoreDetailsDescription =
    RankFusionPipelineBuilder::kRankFusionScoreDetailsDescription;

// Per-pipeline scoreDetails scalar field suffix used in the desugared $group output (and the
// matching $replaceRoot wrapper). For $rankFusion the per-pipeline scalar is "<p>_rank".
inline constexpr std::string_view kDetailsScalarSuffix = "_rank"sv;

// {$_internalSetWindowFields: {sortBy: {order: 1},
//                              output: {<INTERNAL_FIELDS>.<p>_rank: {$rank: {}}}}}
BSONObj buildSetWindowFieldsBson(const std::string& rankFieldName);

// {$addFields: {<INTERNAL_FIELDS>.<p>_score: {$multiply: [{$divide: [1, {$add: [<rank>, K]}]},
//                                                          <weight>]}}}
BSONObj buildScoreAddFieldsBson(std::string_view inputPipelineName,
                                int rankConstant,
                                double weight);

// {$addFields: {<INTERNAL_FIELDS>.<p>_scoreDetails: <three branches>}}
BSONObj buildAddInputPipelineScoreDetailsBson(std::string_view inputPipelineName,
                                              bool inputGeneratesScore,
                                              bool inputGeneratesScoreDetails);

// {$addFields: {<INTERNAL_FIELDS>.<p>_rank: {$cond: [{$eq:[<path>,0]},"NA",<path>]}, ...}}
BSONObj buildRankAddFieldsBson(const std::vector<std::string>& pipelineNames);

// {$setMetadata: {score: {$add: ["$<INTERNAL_FIELDS>.<p>_score", ...]}}} (Full branch)
BSONObj buildSetMetadataScoreBson(const std::vector<std::string>& pipelineNames);

// {$addFields: {score: {$add: ["$<INTERNAL_FIELDS>.<p>_score", ...]}}} (Basic branch)
BSONObj buildAddFieldsScoreBson(const std::vector<std::string>& pipelineNames);

// {$addFields: {<INTERNAL_FIELDS>: {calculatedScoreDetails: [
//      {$mergeObjects: [{inputPipelineName: <p>, rank: "$<p>_rank",
//                        weight: {$cond: [{$eq:["$<p>_rank","NA"]},"$$REMOVE",<weight>]}},
//                       "$<INTERNAL_FIELDS>.<p>_scoreDetails"]},
//      ...]}}}
BSONObj buildCalculatedFinalScoreDetailsBson(const std::vector<std::string>& pipelineNames,
                                             const StringMap<double>& weights);

// {$setMetadata: {scoreDetails: {value: {$meta: "score"}, description: "...",
//                                details: "$<INTERNAL_FIELDS>.calculatedScoreDetails"}}}
BSONObj buildSetMetadataScoreDetailsBson();

// {$sort: {score: -1, _id: 1}} (Basic branch)
BSONObj buildSortByScoreScalarBson();

// Builds the per-input-pipeline preamble for $rankFusion: a clone of the input subpipeline's
// LPDSs (with the rightmost $sort mutated to output sort key metadata) followed by:
//   $replaceWith   ({<INTERNAL_DOCS>: "$$ROOT"})
//   $_internalSetWindowFields ({sortBy: {order: 1}, output: {<rank field>: {$rank: {}}}})
//   $addFields      (per-pipeline weighted score)
//   [optional] $addFields (per-pipeline scoreDetails)
StageSpecs buildRankFusionInputPipelinePreamble(const NamespaceString& nss,
                                                const LiteParsedPipeline& subPipeline,
                                                const std::string& pipelineName,
                                                double weight,
                                                bool includeScoreDetails);

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::rank_fusion_utils
