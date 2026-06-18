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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/score_fusion_pipeline_builder.h"
#include "mongo/util/string_map.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::lite_parsed_hybrid_search_desugarer::score_fusion_utils {

inline constexpr std::string_view kInternalFieldsName =
    ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName;
inline constexpr std::string_view kDocsName = ScoreFusionPipelineBuilder::kScoreFusionDocsFieldName;
inline constexpr std::string_view kScoreDetailsDescription =
    ScoreFusionPipelineBuilder::kScoreFusionScoreDetailsDescription;

// Per-pipeline scoreDetails scalar field suffix used in the desugared $group output (and the
// matching $replaceRoot wrapper). For $scoreFusion the per-pipeline scalar is "<p>_rawScore".
inline constexpr std::string_view kDetailsScalarSuffix = "_rawScore"_sd;

// Validation/translation of normalization + combination spec.
class ScoreFusionScoringOptions {
public:
    explicit ScoreFusionScoringOptions(const ScoreFusionSpec& spec);

    ScoreFusionNormalizationEnum getNormalizationMethod() const {
        return _normalizationMethod;
    }

    std::string_view getNormalizationString() const;

    ScoreFusionCombinationMethodEnum getCombinationMethod() const {
        return _combinationMethod;
    }

    std::string_view getCombinationMethodString() const;

    const boost::optional<IDLAnyType>& getCombinationExpression() const {
        return _combinationExpression;
    }

private:
    ScoreFusionNormalizationEnum _normalizationMethod;
    ScoreFusionCombinationMethodEnum _combinationMethod;
    boost::optional<IDLAnyType> _combinationExpression;
};

// {$addFields: {<INTERNAL_FIELDS>.<p>_score: {$multiply: [<scoreOrNorm>, <weight>]}}}
// where <scoreOrNorm> is determined by the input.normalization:
//   - none / minMaxScaler: {$meta: "score"}
//   - sigmoid: {$sigmoid: {$meta: "score"}}
BSONObj buildScoreAddFieldsBson(std::string_view inputPipelineName,
                                ScoreFusionNormalizationEnum normalization,
                                double weight);

// {$addFields: {<INTERNAL_FIELDS>.<p>_rawScore: {$meta: "score"}}}
BSONObj buildRawScoreAddFieldsBson(std::string_view inputPipelineName);

// {$addFields: {<INTERNAL_FIELDS>.<p>_scoreDetails: ...}} -- two branches:
//   - inputGeneratesScoreDetails: { details: {$meta: "scoreDetails"} }
//   - else                       : { details: [] }
// Mirrors `addInputPipelineScoreDetails` in score_fusion_pipeline_builder.cpp.
BSONObj buildAddInputPipelineScoreDetailsBson(std::string_view inputPipelineName,
                                              bool inputGeneratesScoreDetails);

// {$_internalSetWindowFields: {sortBy: {<INTERNAL_FIELDS>.<p>_score: -1},
//                              output: {<INTERNAL_FIELDS>.<p>_score:
//                                          {$minMaxScaler: {input:
//                                          "$<INTERNAL_FIELDS>.<p>_score"}}}}}
BSONObj buildMinMaxScalerSetWindowFieldsBson(std::string_view inputPipelineName);

// {$setMetadata: {score: {$avg: ["$<INTERNAL_FIELDS>.<p>_score", ...]}}} (avg branch)
// or
// {$setMetadata: {score: {$let: {vars: {<p>: "$<INTERNAL_FIELDS>.<p>_score", ...},
//                                in: <user combination.expression>}}}} (expression branch).
//
// In the expression branch, the user's combination.expression is forwarded as raw BSON. Full
// parse will validate the expression via ExpressionLet::parse.
BSONObj buildSetFinalCombinedScoreBson(const std::vector<std::string>& pipelineNames,
                                       const ScoreFusionScoringOptions& scoringOptions);

// {$addFields: {<INTERNAL_FIELDS>: {calculatedScoreDetails: [
//      {$mergeObjects: [{inputPipelineName: "<p>",
//                        inputPipelineRawScore: "$<INTERNAL_FIELDS>.<p>_rawScore",
//                        weight: <w>,
//                        value: "$<INTERNAL_FIELDS>.<p>_score"},
//                       "$<INTERNAL_FIELDS>.<p>_scoreDetails"]},
//      ...]}}}
BSONObj buildCalculatedFinalScoreDetailsBson(const std::vector<std::string>& pipelineNames,
                                             const StringMap<double>& weights);

// {$setMetadata: {scoreDetails: {value: {$meta: "score"}, description: ...,
//                                normalization: ..., combination: {...}, details: ...}}}
BSONObj buildSetMetadataScoreDetailsBson(const ScoreFusionScoringOptions& scoringOptions);

// Builds the per-input-pipeline preamble for $scoreFusion: a clone of the input subpipeline's
// LPDSs, followed by:
//   $replaceWith   ({<INTERNAL_DOCS>: "$$ROOT"})
//   $addFields      (per-pipeline weighted, optionally normalized, score)
//   [optional] $addFields rawScore + $addFields scoreDetails
//   [optional] $_internalSetWindowFields (minMaxScaler normalization)
StageSpecs buildScoreFusionInputPipelinePreamble(const NamespaceString& nss,
                                                 const LiteParsedPipeline& subPipeline,
                                                 const std::string& pipelineName,
                                                 double weight,
                                                 ScoreFusionNormalizationEnum normalization,
                                                 bool includeScoreDetails);

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::score_fusion_utils
