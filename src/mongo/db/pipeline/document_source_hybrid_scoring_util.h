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

/**
 * Builds and returns a $replaceRoot stage: {$replaceWith: {<INTERNAL_FIELDS_DOCS>: "$$ROOT"}}.
 * This has the effect of storing the unmodified user's document in the path
 * '$_internal_rankFusion_docs' or '$_internal_scoreFusion_docs'.
 */
boost::intrusive_ptr<DocumentSource> buildReplaceRootStage(
    StringData internalFieldsDocsName, const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Produces the BSON for a $group spec to group all the input documents across all pipelines by
 * '_id' and their internal fields (pipeline score, pipeline rank (if $rankFusion)/pipeline rawScore
 * (if $scoreFusion) and pipeline scoreDetails). If a document is not present in an input pipeline,
 * its score for that input pipeline is set to 0.
 *
 * Note, because every field in a $group is defined by its own accumulator,
 * to preserve the structure of all our internal fields being encapsulated by in a single
 * field object, we first in this stage push each document's internal fields in the group
 * into an array using $push.
 *
 * After, the internal fields array is reduced to a single internal fields object
 * that represents the merger of all the internal fields across all documents across
 * all input pipelines of a matching '_id'.
 * Builds a $group like the following
 * (if scoreDetails included; otherwise omit rank (if $rankFusion) or rawScore (if $scoreFusion) and
 * scoreDetails internal fields):
 *
 * {
 *     "$group": {
 *         "_id": "$<INTERNAL_FIELDS_DOCS>._id",
 *         "<INTERNAL_FIELDS_DOCS>": {
 *             "$first": "$<INTERNAL_FIELDS_DOCS>"
 *         },
 *         "<INTERNAL_FIELDS>": {
 *             "$push": {
 *                 "<pipeline1_name>_score": {
 *                     "$ifNull": [
 *                         "$<INTERNAL_FIELDS>.<pipeline1_name>_score", 0
 *                     ]
 *                 },
 *                 "<pipeline1_name>_rank": { // or if $scoreFusion: "<pipeline1_name>_rawScore"
 *                     "$ifNull": [
 *                         "$<INTERNAL_FIELDS>.<pipeline1_name>_rank", 0
 *                         // OR "$<INTERNAL_FIELDS>.<pipeline1_name>_rawScore", 0
 *                     ]
 *                 },
 *                 "<pipeline1_name>_scoreDetails":
 *                    "$<INTERNAL_FIELDS>.<pipeline1_name>_scoreDetails",
 *                 "<pipeline2_name>_score": {
 *                     "$ifNull": [
 *                         "$_<INTERNAL_FIELDS>.<pipeline2_name>_score", 0
 *                 }
 *                 "<pipeline2_name>_rank": {  // or if $scoreFusion: "<pipeline2_name>_rawScore"
 *                     "$ifNull": [
 *                         "$<INTERNAL_FIELDS>.<pipeline2_name>_rank", 0
 *                         // OR "$<INTERNAL_FIELDS>.<pipeline2_name>_rawScore", 0
 *                     ]
 *                 },
 *                 "<pipeline2_name>_scoreDetails":
 *                    "$<INTERNAL_FIELDS>.<pipeline2_name>_scoreDetails"
 *           }
 *        }
 *     }
 *  }
 */
BSONObj groupDocsByIdAcrossInputPipeline(StringData internalFieldsDocsName,
                                         StringData internalFieldsName,
                                         const std::vector<std::string>& pipelineNames,
                                         bool includeScoreDetails);

/**
 * Produces the BSON spec for a $project stage that reduces the <INTERNAL_FIELD> field array,
 * produced after the prior $group by '_id', into a single <INTERNAL_FIELD> object that
 * represents the merged <INTERNAL_FIELD> field objects across all input documents across
 * all input pipelines that have the same '_id'.
 *
 * Conceptually, it does the grouping accumulation of the <INTERNAL_FIELDS> sub-fields
 * of documents of matching '_id's, as $group can not accumulate sub-fields directly.
 *
 * Builds a $project like the following
 * (if scoreDetails included; otherwise omit rank (if $rankFusion) or rawScore (if $scoreFusion) and
 * scoreDetails internal fields):
 *
 * {
 *     "$project": {
 *         "_id": true,
 *         "<INTERNAL_FIELDS_DOCS>": true,
 *         "<INTERNAL_FIELDS>": {
 *             "$reduce": {
 *                 "input": "$<INTERNAL_FIELDS_DOCS>",
 *                 "initialValue": {
 *                     "<pipeline1_name>_score": 0,
 *                     "<pipeline1_name>_rank/rawScore": 0,
 *                     "<pipeline1_name>_scoreDetails": {},
 *                     "<pipeline2_name>_score": 0,
 *                     "<pipeline2_name>_rank/rawScore": 0,
 *                     "<pipeline2_name>_scoreDetails": {}
 *                 },
 *                 "in": {
 *                     "<pipeline1_name>_score": {
 *                         "$max": [
 *                             "$$value.<pipeline1_name>_score",
 *                             "$$this.<pipeline1_name>_score"
 *                         ]
 *                     },
 *                     "<pipeline1_name>_rank/rawScore": {
 *                         "$max": [
 *                             "$$value.<pipeline1_name>_rank/rawScore",
 *                             "$$this.<pipeline1_name>_rank/rawScore"
 *                         ]
 *                     },
 *                     "<pipeline1_name>_scoreDetails": {
 *                         "$mergeObjects": [
 *                             "$$value.<pipeline1_name>_scoreDetails",
 *                             "$$this.<pipeline1_name>_scoreDetails"
 *                         ]
 *                     }
 *                     "<pipeline2_name>_score": {
 *                         "$max": [
 *                             "$$value.<pipeline2_name>_score",
 *                             "$$this.<pipeline2_name>_score"
 *                         ]
 *                     },
 *                     "<pipeline2_name>_rank/rawScore": {
 *                         "$max": [
 *                             "$$value.<pipeline2_name>_rank/rawScore",
 *                             "$$this.<pipeline2_name>_rank/rawScore"
 *                         ]
 *                     },
 *                     "<pipeline2_name>_scoreDetails": {
 *                         "$mergeObjects": [
 *                             "$$value.<pipeline2_name>_scoreDetails",
 *                             "$$this.<pipeline2_name>_scoreDetails"
 *                         ]
 *                     }
 *                 }
 *             }
 *         }
 *     }
 * }
 *
 */
BSONObj projectReduceInternalFields(StringData internalFieldsDocsName,
                                    StringData internalFieldsName,
                                    const std::vector<std::string>& pipelineNames,
                                    bool includeScoreDetails);

/**
 * Builds the following BSON object, in order to promote the user's documents to the top-level while
 * still maintaining the internal processing fields.
 * {
 *   $replaceRoot: {
 *     newRoot: {
 *       $mergeObjects: [
 *         "$<INTERNAL_DOCS>",
 *         "$$ROOT"
 *       ]
 *     }
 *   }
 * }
 */
BSONObj promoteEmbeddedDocsObject(StringData internalFieldsDocsName);

/**
 * Builds the following BSON object, in order to remove the internal docs subobject that hid the
 * user's documents.
 * {
 *   $project: {
 *      <INTERNAL_DOCS>: 0
 *   }
 * }
 */
BSONObj projectRemoveEmbeddedDocsObject(StringData internalFieldsDocsName);

/**
 * Builds the following BSON object, in order to remove the internal processing fields subobject.
 * {
 *   $project: {
 *      <INTERNAL_FIELDS>: 0
 *   }
 * }
 */
BSONObj projectRemoveInternalFieldsObject(StringData internalFieldsName);

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

/*
 * Builds an $addFields stage that constructs the value of the 'details' field array
 * in final top-level 'scoreDetails' object, and stores it in path
 * "$<INTERNAL_FIELDS>.calculatedScoreDetails".
 *
 * Later, this field is used to set the value of the 'details' key when setting the 'scoreDetails'
 * metadata field.
 */
boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    StringData internalFieldsName,
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
