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
