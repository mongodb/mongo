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
#include "mongo/db/pipeline/document_source.h"
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

/**
 * Returns no error if the BSON pipeline is a selection pipeline. A selection pipeline only
 * retrieves a set of documents from a collection, without doing any modifications. For example it
 * cannot do a $project or $replaceRoot.
 */
Status isSelectionPipeline(const std::vector<BSONObj>& bsonPipeline);

/**
 * Returns no error if the BSON stage is a selection stage. A selection stage only retrieves a set
 * of documents from a collection, without doing any modifications.
 */
Status isSelectionStage(const BSONObj& bsonStage);

/**
 * Returns no error if the BSON pipeline is a ranked pipeline. A ranked pipeline is a pipeline that
 * starts with an implicitly ranked stage, or contains an explicit $sort.
 */
Status isRankedPipeline(const std::vector<BSONObj>& bsonPipeline);

/**
 * Returns no error if the BSON pipeline is an scored pipeline. An ordered pipeline is a pipeline
 * that begins with a stage that generates a score, or contains an explicit $score.
 */
Status isScoredPipeline(const std::vector<BSONObj>& bsonPipeline,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Returns true if the BSON pipeline contains a $score stage.
 */
bool pipelineContainsScoreStage(const std::vector<BSONObj>& bsonPipeline);

/**
 * Returns true if the BSON contains a $scoreFusion or $rankFusion and false otherwise.
 */
bool isHybridSearchPipeline(const std::vector<BSONObj>& bsonPipeline);

namespace score_details {
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
                {inputPipelineName: "name2", inputPipelineRawScore: "$name2_rawScore",
                    weight: <weight>, value: "$name2_score"},
                "$name2_scoreDetails"
            ]
        }
        ]
    }}
*/
boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    const std::vector<std::string>& pipelineNames,
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
