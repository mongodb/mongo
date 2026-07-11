// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
using namespace std::literals::string_view_literals;

inline constexpr std::string_view kInternalFieldsName =
    ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName;
inline constexpr std::string_view kDocsName = ScoreFusionPipelineBuilder::kScoreFusionDocsFieldName;
inline constexpr std::string_view kScoreDetailsDescription =
    ScoreFusionPipelineBuilder::kScoreFusionScoreDetailsDescription;

// Per-pipeline scoreDetails scalar field suffix used in the desugared $group output (and the
// matching $replaceRoot wrapper). For $scoreFusion the per-pipeline scalar is "<p>_rawScore".
inline constexpr std::string_view kDetailsScalarSuffix = "_rawScore"sv;

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
