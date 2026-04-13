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
    // Prefix applied to flat field names in the $group stage output. Because $group cannot
    // output dotted-path field names, all accumulated per-pipeline values are stored under
    // "__hs_"-prefixed flat names so they can later be referenced.
    static constexpr StringData kHsFlatFieldPrefix = "__hs_"_sd;

    /**
     * Build a $group and $replaceRoot that aggregate scores across input pipelines and restore
     * user documents to the top level.
     *
     * After all $unionWith branches execute, a document that appears in N input pipelines will
     * have N rows in the stream, each carrying a real score only for its own pipeline. These
     * two stages collapse those rows into a single document per _id.
     *
     * Stage 1 ($group): groups by the user document's _id and uses one accumulator per
     * pipeline field. Because $group cannot output dotted-path field names, accumulated values
     * are stored under flat "__hs_"-prefixed names.
     *
     * Stage 2 ($replaceRoot): promotes the stashed user document from <INTERNAL_DOCS> to the
     * top level, and repacks the flat __hs_* fields into a nested <INTERNAL_FIELDS> object. The
     * $group output (_id, <INTERNAL_DOCS>, __hs_* fields) is implicitly dropped since $replaceRoot
     * replaces the entire document.
     *
     * Output: one document per unique _id with the user's original fields at the top level
     * and per-pipeline scores nested under <INTERNAL_FIELDS>:
     * {_id: 1, title: "...", <INTERNAL_FIELDS>: {<p1>_score: 0.5, <p2>_score: 0.8, ...}}
     *
     * See the .cpp for the full BSON shapes.
     */
    std::list<boost::intrusive_ptr<DocumentSource>> buildGroupAndReplaceRootStages(
        const std::vector<std::string>& pipelineNames,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

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
     * enabled).
     */
    virtual std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
        const std::vector<std::string>& pipelineNames,
        const StringMap<double>& weights,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

    /**
     * Returns the name of the per-pipeline scalar field that carries stage-specific metadata
     * needed for scoreDetails output. Only called when shouldIncludeScoreDetails() is true.
     */
    virtual std::string getScoreDetailsScalarFieldName(StringData pipelineName) const = 0;

    /**
     * Construct the stage-specific fields for each input pipeline to add to the final scoreDetails.
     */
    virtual void constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
        BSONObjBuilder& bob, StringData pipelineName, double weight) = 0;
};

}  // namespace mongo
