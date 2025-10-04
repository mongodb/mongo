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
#include "mongo/db/pipeline/document_source_project.h"
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

BSONObj HybridSearchPipelineBuilder::groupDocsByIdAcrossInputPipeline(
    const std::vector<std::string>& pipelineNames) {
    // For each sub-pipeline, build the following obj:
    // name_score: {$max: {ifNull: ["$name_score", 0]}}
    // If scoreDetails is enabled, build:
    // If $rankFusion:
    // <INTERNAL_FIELDS>.name_rank: {$max: {ifNull: ["$<INTERNAL_FIELDS>.name_rank", 0]}}
    // If $scoreFusion:
    // <INTERNAL_FIELDS>.name_rawScore: {$max: {ifNull: ["$<INTERNAL_FIELDS>.name_rawScore",
    // 0]}} Both $rankFusion and $scoreFusion: <INTERNAL_FIELDS>.name_scoreDetails:
    // {$mergeObjects: $<INTERNAL_FIELDS>.name_scoreDetails}
    BSONObjBuilder bob;
    {
        BSONObjBuilder groupBob(bob.subobjStart("$group"_sd));
        groupBob.append("_id", "$" + getInternalDocsName() + "._id");
        groupBob.append(getInternalDocsName(), BSON("$first" << ("$" + getInternalDocsName())));

        BSONObjBuilder internalFieldsBob(groupBob.subobjStart(getInternalFieldsName()));
        BSONObjBuilder pushBob(internalFieldsBob.subobjStart("$push"_sd));

        for (const auto& pipelineName : pipelineNames) {
            const std::string scoreName =
                hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName);
            pushBob.append(
                scoreName,
                BSON("$ifNull" << BSON_ARRAY(
                         fmt::format("${}",
                                     hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                         getInternalFieldsName(), scoreName))
                         << 0)));
            if (shouldIncludeScoreDetails()) {
                groupDocsByIdAcrossInputPipelineScoreDetails(pipelineName, pushBob);
                const std::string scoreDetailsName = fmt::format("{}_scoreDetails", pipelineName);
                pushBob.append(scoreDetailsName,
                               fmt::format("${}",
                                           hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                               getInternalFieldsName(), scoreDetailsName)));
            }
        }
        pushBob.done();
        internalFieldsBob.done();
        groupBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj HybridSearchPipelineBuilder::projectReduceInternalFields(
    const std::vector<std::string>& pipelineNames) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(getInternalDocsName(), 1);

        BSONObjBuilder internalFieldsBob(projectBob.subobjStart(getInternalFieldsName()));
        BSONObjBuilder reduceBob(internalFieldsBob.subobjStart("$reduce"_sd));

        reduceBob.append("input", "$" + getInternalFieldsName());

        BSONObjBuilder initialValueBob(reduceBob.subobjStart("initialValue"_sd));
        for (const auto& pipelineName : pipelineNames) {
            initialValueBob.append(fmt::format("{}_score", pipelineName), 0);
            if (shouldIncludeScoreDetails()) {
                projectReduceInternalFieldsScoreDetails(initialValueBob, pipelineName, true);
                initialValueBob.append(fmt::format("{}_scoreDetails", pipelineName), BSONObj{});
            }
        }
        initialValueBob.done();

        BSONObjBuilder inBob(reduceBob.subobjStart("in"_sd));
        for (const auto& pipelineName : pipelineNames) {
            inBob.append(
                fmt::format("{}_score", pipelineName),
                BSON("$max" << BSON_ARRAY(fmt::format("$$value.{}_score", pipelineName)
                                          << fmt::format("$$this.{}_score", pipelineName))));
            if (shouldIncludeScoreDetails()) {
                projectReduceInternalFieldsScoreDetails(inBob, pipelineName, false);
                inBob.append(fmt::format("{}_scoreDetails", pipelineName),
                             BSON("$mergeObjects" << BSON_ARRAY(
                                      fmt::format("$$value.{}_scoreDetails", pipelineName)
                                      << fmt::format("$$this.{}_scoreDetails", pipelineName))));
            }
        }
        inBob.done();

        reduceBob.done();
        internalFieldsBob.done();
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj HybridSearchPipelineBuilder::projectRemoveEmbeddedDocsObject() {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(getInternalDocsName(), 0);
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj HybridSearchPipelineBuilder::promoteEmbeddedDocsObject() {
    BSONObjBuilder bob;
    {
        BSONObjBuilder replaceRootBob(bob.subobjStart("$replaceRoot"_sd));
        BSONObjBuilder newRootBob(replaceRootBob.subobjStart("newRoot"_sd));

        BSONArrayBuilder mergeObjectsArrayBab;
        mergeObjectsArrayBab.append("$" + getInternalDocsName());
        mergeObjectsArrayBab.append("$$ROOT");
        mergeObjectsArrayBab.done();

        newRootBob.append("$mergeObjects", mergeObjectsArrayBab.arr());
        newRootBob.done();
        replaceRootBob.done();
    }
    bob.done();
    return bob.obj();
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

            auto collName = pExpCtx->getUserNss().coll();

            BSONObj inputToUnionWith =
                BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
            auto unionWithStage =
                DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), pExpCtx);
            outputStages.emplace_back(unionWithStage);
        }
    }
    // Build all remaining stages to perform the fusion. After all the pipelines have been
    // executed and unioned, builds the $group stage to merge the scoreFields/apply score nulls
    // behavior.
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;
    // Group all the documents across the different $unionWiths for each input pipeline.
    scoreAndMergeStages.emplace_back(DocumentSourceGroup::createFromBson(
        groupDocsByIdAcrossInputPipeline(pipelineNames).firstElement(), pExpCtx));

    // Combine all internal processing fields into one blob.
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        projectReduceInternalFields(pipelineNames).firstElement(), pExpCtx));

    // Promote the user's documents back to the top-level so that we can evaluate the expression
    // potentially using fields from the user's documents.
    scoreAndMergeStages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        promoteEmbeddedDocsObject().firstElement(), pExpCtx));
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        projectRemoveEmbeddedDocsObject().firstElement(), pExpCtx));

    // Call the stage-specific merge logic to add the remaining desugaring stages.
    auto restOfScoreAndMergeStages = buildScoreAndMergeStages(pipelineNames, getWeights(), pExpCtx);
    scoreAndMergeStages.splice(scoreAndMergeStages.end(), std::move(restOfScoreAndMergeStages));
    outputStages.splice(outputStages.end(), std::move(scoreAndMergeStages));
    return outputStages;
}
}  // namespace mongo
