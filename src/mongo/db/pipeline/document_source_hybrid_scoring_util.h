/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/pipeline/document_source_rank_fusion_inputs_gen.h"
#include "mongo/db/pipeline/document_source_score_fusion_inputs_gen.h"

namespace mongo::hybrid_scoring_util {

/**
 * Checks if this stage is a $score stage, where it has been desugared to $setMetadata with the meta
 * type MetaType::kScore.
 */
bool isScoreStage(const boost::intrusive_ptr<DocumentSource>& stage);

/**
 * Return pipeline's associated weight, if it exists. Otherwise, return a default of 1.
 */
double getPipelineWeight(const StringMap<double>& weights, const std::string& pipelineName);

/**
 * Verifies that each entry in inputWeights specifies a numerical weight value associated with a
 * unique and valid pipeline name from inputPipelines. inputWeights has the following structure:
 * {"pipelineName": weightVal} where "pipelineName" is a string and weightVal is a double. Returns a
 * map from the pipeline name to the specified weight (as a double) for that pipeline.
 * Note: not all pipelines must be in the returned map. This means any valid subset from none to all
 * of the pipelines may be contained in the resulting map. Any pipelines not present in the
 * resulting map have an implicit default weight of 1.
 */
StringMap<double> validateWeights(
    const mongo::BSONObj& inputWeights,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    StringData stageName);

/**
 * This function will fail the query in the case where nonexistent pipelines were referenced in the
 * weights. Before failing the query outright, the function first computes the best valid, unmatched
 * pipeline the user could have intended for each invalid weight and builds it into a
 * user-friendly error message to give the best possible feedback.
 *
 * Note: This function needs a list of the unmatched pipelines, but is instead given a list of
 *       all pipelines and matched pipelines, which can be used to compute the unmatched pipelines.
 *       This is for performance reasons, because the caller of this function can easily know these
 *       inputs, and only needs to call this function in error cases.
 */
void failWeightsValidationWithPipelineSuggestions(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& allPipelines,
    const stdx::unordered_set<std::string>& matchedPipelines,
    const std::vector<std::string>& invalidWeights,
    StringData stageName);

namespace score_details {

/**
 * Builds and returns an $addFields stage that materializes scoreDetails for an individual input
 * pipeline. The way we materialize scoreDetails depends on if the input pipeline generates "score"
 * or "scoreDetails" metadata.
 *
 * Later, these individual input pipeline scoreDetails will be gathered together in order to build
 * scoreDetails for the overall $rankFusion pipeline (see calculateFinalScoreDetails()).
 */
boost::intrusive_ptr<DocumentSource> addScoreDetails(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    StringData inputPipelinePrefix,
    bool inputGeneratesScore,
    bool inputGeneratesScoreDetails);

/**
 * Construct the scoreDetails field name and obj (ex: name_scoreDetails: {$mergeObjects:
 * $name_scoreDetails}) for the grouping stage.
 */
std::pair<std::string, BSONObj> constructScoreDetailsForGrouping(std::string pipelineName);

// Calculate the final scoreDetails field for the entire stage. If rankFusion is false, then the
// object for scoreFusion gets generated. Creates the following object:
// For rankFusion:
/*
    { $addFields: {
        calculatedScoreDetails: [
        {
            $mergeObjects: [
                {inputPipelineName: "name1", rank: "$name1_rank",
                    weight: <weight>},
                "$name1_scoreDetails"
            ]
        },
*/
// For scoreFusion:
/*
    { $addFields: {
        calculatedScoreDetails: [
        {
            $mergeObjects: [
                {inputPipelineName: "name2", inputPipelineRawScore: "$name2_inputPipelineRawScore",
                    weight: <weight>},
                "$name2_scoreDetails"
            ]
        }
        ]
    }}
*/
boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs,
    const StringMap<double>& weights,
    bool isRankFusion,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Returns the stringified verion of a given expression with the following format:
 * "string": {"stringified expression"}
 */
std::string stringifyExpression(boost::optional<IDLAnyType> expression);
}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
