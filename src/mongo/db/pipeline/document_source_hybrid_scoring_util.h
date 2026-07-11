// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::hybrid_scoring_util {
using namespace std::literals::string_view_literals;

static constexpr std::string_view kIsHybridSearchFlagFieldName = "$_internalIsHybridSearch"sv;

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
    std::string_view stageName);

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
    std::string_view stageName);

/**
 * Overload taking the unmatched pipeline names directly, for callers (such as the lite-parsed
 * desugarer) that operate on pipeline names rather than parsed Pipelines. Both overloads throw the
 * same error so the full-parse and lite-parse paths fail identically.
 */
void failWeightsValidationWithPipelineSuggestions(
    const std::vector<std::string>& unmatchedPipelines,
    const std::vector<std::string>& invalidWeights,
    std::string_view stageName);

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
 * flag set. Asserts with error 5491300 if a non-internal client supplied it.
 *
 * Note the explain-on-a-view interaction: the router desugars the subpipeline and, when dispatching
 * to shards, serializes $_internalIsHybridSearch into the BSON. For a view the shards respond that
 * the view must be executed on the router, and the router retries with the fully-desugared pipeline
 * -- where the client is not internal. To keep this retry from tripping the assertion, $lookup and
 * $unionWith omit $_internalIsHybridSearch from their explain serialization.
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
inline std::string getScoreFieldFromPipelineName(std::string_view pipelineName,
                                                 bool includeDollarSign = false) {
    return includeDollarSign ? fmt::format("${}_score", pipelineName)
                             : fmt::format("{}_score", pipelineName);
}

/**
 * Returns the name of the given value with the given internalFieldsName prefix concatenated to it.
 * Ex: <INTERNAL_FIELDS_NAME>.<value> // "_internal_rankFusion_internal_fields.inputPipelineRank"
 */
inline std::string applyInternalFieldPrefixToFieldName(std::string_view internalFieldsName,
                                                       std::string_view value) {
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
