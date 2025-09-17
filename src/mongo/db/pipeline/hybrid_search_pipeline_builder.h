/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * All hybrid search stages are implemented as a desugared list of other non-hybrid search stages.
 * Furthermore, all hybrid search desugars follow this structure for building the desugared output
 * for a query with N input pipelines: stages for the first input pipeline, N -1 $unionWith stages
 * for combining the subsequent input pipelines, a list of stages that group, score, and order the
 * final results set.
 *
 * HybridSearchPipelineBuilder seeks to modularize this concept, by defining virtual methods for the
 * specifics of how each hybrid search stage handles its subcomponents of this common desugaring
 * structure, and then exposes the main constructDesugaredOutput() method that produces the total
 * list of desugared stages agnostically.
 */
class MONGO_MOD_PRIVATE HybridSearchPipelineBuilder {
public:
    /**
     * Returns a list of stages that represent the final desugared state of a hybrid search stage.
     */
    std::list<boost::intrusive_ptr<DocumentSource>> constructDesugaredOutput(
        const std::map<std::string, std::unique_ptr<Pipeline>>& inputPipelines,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

protected:
    const StringMap<double> _weights;
    const StringData _stageInternalFieldsName;
    const StringData _stageInternalDocsName;
    const bool _includeScoreDetails;
    const StringData _scoreDetailsDescription;

    StringMap<double> getWeights() const {
        return _weights;
    }

    StringData getInternalFieldsName() const {
        return _stageInternalFieldsName;
    }

    StringData getInternalDocsName() const {
        return _stageInternalDocsName;
    }

    bool shouldIncludeScoreDetails() const {
        return _includeScoreDetails;
    }

    StringData getScoreDetailsDescription() const {
        return _scoreDetailsDescription;
    }

    /**
     * Builds and returns a $replaceRoot stage: {$replaceWith: {<INTERNAL_FIELDS_DOCS>: "$$ROOT"}}.
     * This has the effect of storing the unmodified user's document in the path
     * '$_internal_rankFusion_docs' or '$_internal_scoreFusion_docs'.
     */
    boost::intrusive_ptr<DocumentSource> buildReplaceRootStage(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Builds the following BSON object, in order to remove the internal processing fields
     * subobject.
     * {
     *   $project: {
     *      <INTERNAL_FIELDS>: 0
     *   }
     * }
     */
    BSONObj projectRemoveInternalFieldsObject();

    /*
     * Builds an $addFields stage that constructs the value of the 'details' field array
     * in final top-level 'scoreDetails' object, and stores it in path
     * "$<INTERNAL_FIELDS>.calculatedScoreDetails".
     *
     * Later, this field is used to set the value of the 'details' key when setting the
     * 'scoreDetails' metadata field.
     */
    boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
        const std::vector<std::string>& pipelineNames,
        const StringMap<double>& weights,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    HybridSearchPipelineBuilder(StringMap<double> weights,
                                StringData stageInternalFieldsName,
                                StringData stageInternalDocsName,
                                bool includeScoreDetails,
                                StringData scoreDetailsDescription)
        : _weights(weights),
          _stageInternalFieldsName(stageInternalFieldsName),
          _stageInternalDocsName(stageInternalDocsName),
          _includeScoreDetails(includeScoreDetails),
          _scoreDetailsDescription(scoreDetailsDescription) {}

private:
    /**
     * Produces the BSON for a $group spec to group all the input documents across all pipelines by
     * '_id' and their internal fields (pipeline score, pipeline rank (if $rankFusion)/pipeline
     * rawScore (if $scoreFusion) and pipeline scoreDetails). If a document is not present in an
     * input pipeline, its score for that input pipeline is set to 0.
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
     * (if scoreDetails included; otherwise omit rank (if $rankFusion) or rawScore (if $scoreFusion)
     * and scoreDetails internal fields):
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
     *                 "<pipeline2_name>_rank": {  // or if $scoreFusion:
     * "<pipeline2_name>_rawScore"
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
    BSONObj groupDocsByIdAcrossInputPipeline(const std::vector<std::string>& pipelineNames);

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
     * (if scoreDetails included; otherwise omit rank (if $rankFusion) or rawScore (if $scoreFusion)
     * and scoreDetails internal fields):
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
    BSONObj projectReduceInternalFields(const std::vector<std::string>& pipelineNames);

    /**
     * Builds the following BSON object, in order to promote the user's documents to the top-level
     * while still maintaining the internal processing fields.
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
    BSONObj promoteEmbeddedDocsObject();

    /**
     * Builds the following BSON object, in order to remove the internal docs subobject that hid the
     * user's documents.
     * {
     *   $project: {
     *      <INTERNAL_DOCS>: 0
     *   }
     * }
     */
    BSONObj projectRemoveEmbeddedDocsObject();

    /**
     * Build stages for beginning of input pipeline. For $rankFusion, will preserve rank, weight
     * the score, and preserve scoreDetails (if the user enabled it). For $scoreFusion, will
     * preserve rawScore, weight the score, normalize the score, and preserve scoreDetails (if the
     * user enabled it).
     */
    virtual std::list<boost::intrusive_ptr<DocumentSource>> buildInputPipelineDesugaringStages(
        StringData firstInputPipelineName,
        double weight,
        const std::unique_ptr<Pipeline>& pipeline,
        bool inputGeneratesScoreDetails,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

    /**
     * Build the stages that group the documents by _id across all input pipelines, sorts the
     * documents, removes internal processing fields, and calculates the final scoreDetails (if
     * enabled). final scoreDetails
     */
    virtual std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
        const std::vector<std::string>& pipelineNames,
        const StringMap<double>& weights,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

    /**
     * Adds the scoreNulls behavior for stage-specific fields to the $group stage.
     */
    virtual void groupDocsByIdAcrossInputPipelineScoreDetails(StringData pipelineName,
                                                              BSONObjBuilder& pushBob) = 0;

    /**
     * Remove the stage-specific internal processing fields.
     */
    virtual void projectReduceInternalFieldsScoreDetails(BSONObjBuilder& bob,
                                                         StringData pipelineName,
                                                         bool forInitialValue) = 0;

    /**
     * Construct the stage-specific fields for each input pipeline to add to the final scoreDetails.
     */
    virtual void constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
        BSONObjBuilder& bob, StringData pipelineName, double weight) = 0;
};

}  // namespace mongo
