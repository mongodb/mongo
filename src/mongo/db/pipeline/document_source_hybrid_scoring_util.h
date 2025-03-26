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
 * Checks if this pipeline will generate score metadata.
 */
bool isScoredPipeline(const Pipeline& pipeline);

/**
 * Return pipeline's associated weight, if it exists. Otherwise, return a default of 1.
 */
double getPipelineWeight(const StringMap<double>& weights, const std::string& pipelineName);

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
    const std::string& prefix,
    bool inputGeneratesScore,
    bool inputGeneratesScoreDetails);

/**
 * Construct the scoreDetails field name and obj (ex: name_scoreDetails: {$mergeObjects:
 * $name_scoreDetails}) for the grouping stage.
 */
std::pair<std::string, BSONObj> constructScoreDetailsForGrouping(std::string pipelineName);

// Calculate the final scoreDetails field for the entire stage. Creates the following object:
/*
    { $addFields: {
        calculatedScoreDetails: [
        {
            $mergeObjects: [
                {inputPipelineName: "name1", rank: "$name1_rank", weight: <weight>},
                "$name1_scoreDetails"
            ]
        },
        {
            $mergeObjects: [
                {inputPipelineName: "name2", rank: "$name2_rank", weight: <weight>},
                "$name2_scoreDetails"
            ]
        },
        ...
        ]
    }}
*/
boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Constuct the scoreDetails metadata object. Looks like the following:
 * { "$setMetadata": {"scoreDetails": {"value": "$score", "description":
 * {"scoreDetailsDescription..."}, "details": "$calculatedScoreDetails"}}},
 */
boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const std::string& scoreDetailsDescription,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);
}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
