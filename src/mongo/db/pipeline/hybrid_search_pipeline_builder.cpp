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

#include "mongo/db/pipeline/hybrid_search_pipeline_builder.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo {

boost::intrusive_ptr<DocumentSource> HybridSearchPipelineBuilder::buildReplaceRootStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON(getInternalDocsName() << "$$ROOT")).firstElement(), expCtx);
}

BSONObj HybridSearchPipelineBuilder::projectRemoveInternalFieldsObject() {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(getInternalFieldsName(), 0);
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

boost::intrusive_ptr<DocumentSource>
HybridSearchPipelineBuilder::constructCalculatedFinalScoreDetails(
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            BSONObjBuilder internalFieldsBob(addFieldsBob.subobjStart(getInternalFieldsName()));
            {
                BSONArrayBuilder calculatedScoreDetailsArr;
                for (const auto& pipelineName : pipelineNames) {
                    const std::string internalFieldsPipelineName =
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            getInternalFieldsName(), pipelineName);
                    const std::string scoreDetailsFieldName =
                        fmt::format("${}_scoreDetails", pipelineName);
                    double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);
                    BSONObjBuilder mergeObjectsArrSubObj;
                    mergeObjectsArrSubObj.append("inputPipelineName"_sd, pipelineName);
                    constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
                        mergeObjectsArrSubObj, internalFieldsPipelineName, weight);
                    mergeObjectsArrSubObj.done();
                    BSONArrayBuilder mergeObjectsArr;
                    mergeObjectsArr.append(mergeObjectsArrSubObj.obj());
                    mergeObjectsArr.append(
                        fmt::format("${}.{}_scoreDetails", getInternalFieldsName(), pipelineName));
                    mergeObjectsArr.done();
                    BSONObj mergeObjectsObj = BSON("$mergeObjects"_sd << mergeObjectsArr.arr());
                    calculatedScoreDetailsArr.append(mergeObjectsObj);
                }
                calculatedScoreDetailsArr.done();
                internalFieldsBob.append("calculatedScoreDetails", calculatedScoreDetailsArr.arr());
            }
            internalFieldsBob.done();
        }
    }
    const BSONObj spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}


std::list<boost::intrusive_ptr<DocumentSource>>
HybridSearchPipelineBuilder::constructDesugaredOutput(
    const std::map<std::string, std::unique_ptr<Pipeline>>& inputPipelines,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // It is currently necessary to annotate on the ExpressionContext that this is a
    // hybrid search ($rankFusion or $scoreFusion) query. Once desugaring happens, there's no
    // way to identity from the (desugared) pipeline alone that it came from hybrid search. We
    // need to know if it came from hybrid search so we can reject the query if it is run over a
    // view.

    // This flag's value is also used to gate an internal client error. See
    // search_helper::validateViewNotSetByUser(...) for more details.
    pExpCtx->setIsHybridSearch();

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    // Array to store pipeline names separately because Pipeline objects in the 'inputPipelines'
    // map will be moved eventually to other structures, rendering 'inputPipelines' unusable.
    // With this array, we can safely use/pass the pipeline names information without using
    // 'inputPipelines'. Note that pipeline names are stored in the same order in which
    // pipelines are desugared.
    std::vector<std::string> pipelineNames;
    for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
         pipeline_it++) {
        const auto& [inputPipelineName, inputPipelineStages] = *pipeline_it;

        pipelineNames.push_back(inputPipelineName);

        // Check if an explicit weight for this pipeline has been specified.
        // If not, the default is one.
        double pipelineWeight =
            hybrid_scoring_util::getPipelineWeight(getWeights(), inputPipelineName);

        const bool inputGeneratesScoreDetails =
            inputPipelineStages->generatesMetadataType(DocumentMetadataFields::kScoreDetails);

        auto initialStagesInInputPipeline =
            buildInputPipelineDesugaringStages(inputPipelineName,
                                               pipelineWeight,
                                               std::move(inputPipelineStages),
                                               inputGeneratesScoreDetails,
                                               pExpCtx);
        if (pipeline_it == inputPipelines.begin()) {
            // Stages for the first pipeline.
            outputStages.splice(outputStages.end(), std::move(initialStagesInInputPipeline));
        } else {
            // For the input pipelines other than the first, we wrap them in a $unionWith stage
            // to append it to the total desugared output. The input pipeline consists of the
            // same stages returned by 'buildInputPipelineDesugaringStages'.
            auto unionWithPipeline = Pipeline::create(initialStagesInInputPipeline, pExpCtx);
            std::vector<BSONObj> bsonPipeline = unionWithPipeline->serializeToBson();

            // TODO SERVER-121091 This should have been moved into the LiteParsedDesugarer so the
            // check should be redundant.
            auto ifrCtx = pExpCtx->getIfrContext();
            auto hybridSearchFlagEnabled = ifrCtx &&
                ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
            if (hybridSearchFlagEnabled) {
                auto unionNss = pExpCtx->getUserNss();
                UnionWithStageParams params(
                    std::move(unionNss), std::move(bsonPipeline), false, true, BSONElement());
                auto docSources = DocumentSourceUnionWith::createFromStageParams(params, pExpCtx);
                outputStages.emplace_back(docSources.front());
            } else {
                auto collName = pExpCtx->getUserNss().coll();
                BSONObj inputToUnionWith =
                    BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
                auto unionWithStage = DocumentSourceUnionWith::createFromBson(
                    inputToUnionWith.firstElement(), pExpCtx);
                outputStages.emplace_back(unionWithStage);
            }
        }
    }
    // Build all remaining stages to perform the fusion. After all the pipelines have been
    // executed and unioned, builds the $group stage to merge the scoreFields/apply score nulls
    // behavior.
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;
    // Group all the documents across the different $unionWiths for each input pipeline,
    // then reshape the grouped output to restore user documents at the top level.
    auto groupAndShapeStages = buildGroupAndReplaceRootStages(pipelineNames, pExpCtx);
    scoreAndMergeStages.splice(scoreAndMergeStages.end(), std::move(groupAndShapeStages));

    // Call the stage-specific merge logic to add the remaining desugaring stages.
    auto restOfScoreAndMergeStages = buildScoreAndMergeStages(pipelineNames, getWeights(), pExpCtx);
    scoreAndMergeStages.splice(scoreAndMergeStages.end(), std::move(restOfScoreAndMergeStages));
    outputStages.splice(outputStages.end(), std::move(scoreAndMergeStages));
    return outputStages;
}

std::list<boost::intrusive_ptr<DocumentSource>>
HybridSearchPipelineBuilder::buildGroupAndReplaceRootStages(
    const std::vector<std::string>& pipelineNames,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const std::string internalDocsName{getInternalDocsName()};
    const std::string internalFieldsName{getInternalFieldsName()};
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    // Stage 1: $group — collapses multiple rows per _id into one document.
    //
    // Each row entering the $group came from one input pipeline, so only that pipeline's
    // fields have real values; the rest are null. Gets the value using $max for scalar fields
    // and $mergeObjects for scoreDetails. $group cannot output dotted paths, so all values use flat
    // "__hs_"-prefixed names to differentiate these fields for references in the $replaceRoot
    // stage.
    //
    // {$group: {
    //     _id: "$<INTERNAL_DOCS>._id",
    //     <INTERNAL_DOCS>: {$first: "$<INTERNAL_DOCS>"},
    //     __hs_<p1>_score: {$max: {$ifNull: ["$<INTERNAL_FIELDS>.<p1>_score", 0]}},
    //     *** only when scoreDetails is enabled ***
    //     __hs_<p1>_rank/_rawScore: {$max: {$ifNull: ["$<INTERNAL_FIELDS>.<p1>_rank",0]}},
    //     __hs_<p1>_scoreDetails: {$mergeObjects: "$<INTERNAL_FIELDS>.<p1>_scoreDetails"},
    //     ***
    //     __hs_<p2>_score: ...,
    //     ...
    // }}
    BSONObjBuilder groupSpecBob;
    {
        BSONObjBuilder gBob(groupSpecBob.subobjStart("$group"_sd));
        gBob.append("_id", fmt::format("${}._id", internalDocsName));
        gBob.append(internalDocsName, BSON("$first" << fmt::format("${}", internalDocsName)));

        auto accumulateScalarField = [&](StringData field, const std::string& internalPath) {
            gBob.append(fmt::format("{}{}", kHsFlatFieldPrefix, field),
                        BSON("$max" << BSON("$ifNull"
                                            << BSON_ARRAY(fmt::format("${}", internalPath) << 0))));
        };

        for (const auto& pipelineName : pipelineNames) {
            const std::string scoreField =
                hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName);
            accumulateScalarField(scoreField,
                                  hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                      internalFieldsName, scoreField));

            if (shouldIncludeScoreDetails()) {
                const std::string scalarField = getScoreDetailsScalarFieldName(pipelineName);
                accumulateScalarField(scalarField,
                                      hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                          internalFieldsName, scalarField));

                const std::string scoreDetailsField = fmt::format("{}_scoreDetails", pipelineName);
                const std::string internalScoreDetailsPath =
                    hybrid_scoring_util::applyInternalFieldPrefixToFieldName(internalFieldsName,
                                                                             scoreDetailsField);
                gBob.append(fmt::format("{}{}", kHsFlatFieldPrefix, scoreDetailsField),
                            BSON("$mergeObjects" << fmt::format("${}", internalScoreDetailsPath)));
            }
        }
        gBob.done();
    }
    stages.emplace_back(
        DocumentSourceGroup::createFromBson(groupSpecBob.obj().firstElement(), expCtx));

    // Stage 2: $replaceRoot — replaces the $group output with a new document.
    //
    // $mergeObjects takes two arguments: (a) the stashed user document, whose fields get
    // spread to the top level and (b) an object literal that reads the flat __hs_* fields via
    // "$__hs_..." path expressions and nests them under <INTERNAL_FIELDS>. Since $replaceRoot
    // replaces the entire document, the $group output is implicitly dropped.
    //
    // {$replaceRoot: {newRoot: {$mergeObjects: [
    //     "$<INTERNAL_DOCS>",
    //     {<INTERNAL_FIELDS>: {
    //         <p1>_score: "$__hs_<p1>_score",
    //         <p2>_score: "$__hs_<p2>_score",
    //         ...
    //     }}
    // ]}}}

    BSONObjBuilder rrSpecBob;
    {
        BSONObjBuilder rrBob(rrSpecBob.subobjStart("$replaceRoot"_sd));
        BSONObjBuilder newRootBob(rrBob.subobjStart("newRoot"_sd));
        {
            BSONArrayBuilder mergeArr(newRootBob.subarrayStart("$mergeObjects"_sd));
            // Add 'internalDocsName' to promote user doc.
            mergeArr.append(fmt::format("${}", internalDocsName));
            BSONObjBuilder wrapperBob;
            {
                BSONObjBuilder internalFieldsBob(wrapperBob.subobjStart(internalFieldsName));

                auto appendFlatRef = [&](StringData field) {
                    internalFieldsBob.append(field,
                                             fmt::format("${}{}", kHsFlatFieldPrefix, field));
                };

                // Get prefixed fields to nest under 'internalFieldsName'.
                for (const auto& pipelineName : pipelineNames) {
                    appendFlatRef(hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName));
                    if (shouldIncludeScoreDetails()) {
                        appendFlatRef(getScoreDetailsScalarFieldName(pipelineName));
                        appendFlatRef(fmt::format("{}_scoreDetails", pipelineName));
                    }
                }
                internalFieldsBob.done();
            }
            mergeArr.append(wrapperBob.obj());
            mergeArr.done();
        }
        newRootBob.done();
        rrBob.done();
    }
    stages.emplace_back(
        DocumentSourceReplaceRoot::createFromBson(rrSpecBob.obj().firstElement(), expCtx));

    return stages;
}

}  // namespace mongo
