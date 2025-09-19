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
#include "mongo/util/modules.h"

namespace mongo::hybrid_scoring_util {

static constexpr StringData kIsHybridSearchFlagFieldName = "$_internalIsHybridSearch"_sd;

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
    const std::map<std::string, std::unique_ptr<Pipeline>>& inputPipelines,
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
    const std::map<std::string, std::unique_ptr<Pipeline>>& allPipelines,
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

/**
 * Validates that the provided spec does not have the internal-use-only $_internalIsHybridSearch
 * flag set.
 *
 * TODO SERVER-108117 This is currently not called because the validation is broken when running an
 * explain on a view in a sharded collection. In that scenario, the router desugars the subpipeline,
 * adds $_internalIsHybridSearch to the serialized BSON, and sends it to the shards. The shards
 * respond with an error that the view must be executed on the router, and then the router tries
 * executing the fully-desugared pipeline. However, on this retry, the internal client flag is not
 * set, and the router fails the explain due to this assertion.
 */
void validateIsHybridSearchNotSetByUser(boost::intrusive_ptr<ExpressionContext> expCtx,
                                        const BSONObj& spec);

/**
 * Validates that a given collection/view namespace is not a timeseries collection for hybrid
 * search.
 */
void assertForeignCollectionIsNotTimeseries(const NamespaceString& nss,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx);

// -----------------------------Helper String Manipulation Functions-----------------------------

/**
 * Returns either the name of the score field or the scorePath if includeDollarSign is true.
 */
inline std::string getScoreFieldFromPipelineName(StringData pipelineName,
                                                 bool includeDollarSign = false) {
    return includeDollarSign ? fmt::format("${}_score", pipelineName)
                             : fmt::format("{}_score", pipelineName);
}

/**
 * Returns the name of the given value with the given internalFieldsName prefix concatenated to it.
 * Ex: <INTERNAL_FIELDS_NAME>.<value> // "_internal_rankFusion_internal_fields.inputPipelineRank"
 */
inline std::string applyInternalFieldPrefixToFieldName(StringData internalFieldsName,
                                                       StringData value) {
    return fmt::format("{}.{}", internalFieldsName, value);
}

namespace score_details {
/**
 * Returns the stringified verion of a given expression with the following format:
 * "string": {"stringified expression"}
 */
std::string stringifyExpression(boost::optional<IDLAnyType> expression);

/*
 * Builds an $addFields stage that constructs the value of the 'details' field array
 * in final top-level 'scoreDetails' object, and stores it in path
 * "$<INTERNAL_FIELDS>.calculatedScoreDetails".
 *
 * Later, this field is used to set the value of the 'details' key when setting the 'scoreDetails'
 * metadata field.
 */

}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
