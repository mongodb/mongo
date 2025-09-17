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

#include "mongo/db/pipeline/rank_fusion_pipeline_builder.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/rank_fusion_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo {
// Below are helper functions that return stages or stage lists that represent sub-components
// of the total $rankFusion desugar. They are defined in an order close to how
// they appear in the desugar read from top to bottom.
// Generally, during the intermediate processing of $rankFusion, the docs moving through
// the pipeline look like:
// {
//   "_id": ...,
//   "<INTERNAL_FIELDS_DOCS>": { <unmodified document from collection> },
//   "<INTERNAL_FIELDS>"; { <internal variable for intermediate processing > }
// }

/**
 * Searches for a sort stage and request that sort stage outputs sort key metadata for each result.
 */
auto makeSureSortKeyIsOutput(const auto& stageList) {
    DocumentSourceSort* rightMostSort = nullptr;
    for (auto&& stage : stageList) {
        if (auto sortStage = dynamic_cast<DocumentSourceSort*>(stage.get())) {
            rightMostSort = sortStage;
        }
    }
    if (rightMostSort)
        rightMostSort->pleaseOutputSortKeyMetadata();
}

/**
 * Outputs a $setWindowFields stage that sets the $rank value.
 */
boost::intrusive_ptr<DocumentSource> setWindowFields(const auto& expCtx,
                                                     const std::string& rankFieldName) {
    // TODO SERVER-98562 We shouldn't need to provide this sort, since it's not used other than to
    // pass the parse-time validation checks.
    const SortPattern dummySortPattern{BSON("order" << 1), expCtx};
    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        boost::none,  // partitionBy
        dummySortPattern,
        std::vector<WindowFunctionStatement>{WindowFunctionStatement{
            rankFieldName,
            window_function::Expression::parse(
                BSON("$rank" << BSONObj()), dummySortPattern, expCtx.get())}},
        SbeCompatibility::notCompatible);
}

/**
 * Builds and returns an $addFields stage that computes the weighted and normalized score
 * of this input pipeline. The computed score is stored in the field
 * "<INTERNAL_FIELDS>.<inputPipelineName>_score".
 * { $addFields:
 *     { <INTERNAL_FIELDS>.<inputPipelineName>_score:
 *         { multiply:
 *             [ { $divide: [1, { $add: [rank, rankConstant] } ] } ]
 *         },
 *     }
 * }
 */
boost::intrusive_ptr<DocumentSource> buildScoreAddFieldsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelineName,
    const int rankConstant,
    const double weight) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            const std::string internalFieldsInputPipelineScoreName =
                hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                    RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
                    fmt::format("{}_score", inputPipelineName));
            BSONObjBuilder scoreField(
                addFieldsBob.subobjStart(internalFieldsInputPipelineScoreName));
            {
                BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
                // RRF Score = weight * (1 / (rank + rank constant)).
                multiplyArray.append(BSON(
                    "$divide" << BSON_ARRAY(
                        1 << BSON(
                            "$add" << BSON_ARRAY(
                                fmt::format(
                                    "${}",
                                    hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                        RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
                                        fmt::format("{}_rank", inputPipelineName)))
                                << rankConstant)))));
                multiplyArray.append(weight);
            }
        }
    }

    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns an $addFields stage that materializes scoreDetails for an individual input
 * pipeline. The way we materialize scoreDetails depends on if the input pipeline generates "score"
 * or "scoreDetails" metadata.
 *
 * Later, these individual input pipeline scoreDetails will be gathered together in order to build
 * scoreDetails for the overall $rankFusion pipeline (see calculateFinalScoreDetails()).
 */
boost::intrusive_ptr<DocumentSource> addInputPipelineScoreDetails(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelineName,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails) {
    const std::string scoreDetails = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
        RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
        fmt::format("{}_scoreDetails", inputPipelineName));
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));

        if (inputGeneratesScoreDetails) {
            // If the input pipeline generates scoreDetails (for example, $search may generate
            // searchScoreDetails), then we'll use the existing details:
            // { $addFields: { <INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: { $meta:
            // "scoreDetails"} } } We don't grab { $meta: "score" } because we assume any existing
            // scoreDetails already includes its own score at "scoreDetails.value".
            addFieldsBob.append(scoreDetails, BSON("$meta" << "scoreDetails"));
        } else if (inputGeneratesScore) {
            // If the input pipeline does not generate scoreDetails but does generate a "score" (for
            // example, a $text query sorted on the text score), we'll build our own scoreDetails
            // for the pipeline like:
            // { $addFields: { <INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: { value: { $meta:
            // "score" }, details: [] } } }
            addFieldsBob.append(
                scoreDetails,
                BSON("value" << BSON("$meta" << "score") << "details" << BSONArrayBuilder().arr()));
        } else {
            // If the input pipeline generates neither "score" not "scoreDetails" (for example, a
            // pipeline with just a $sort), we don't have any interesting information to include in
            // scoreDetails (rank is added later). We'll still build empty scoreDetails to
            // reflect that:
            // { $addFields: { <INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: { details: [] } }
            // }
            addFieldsBob.append(scoreDetails, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns an $addFields stage that sets the rank to "NA" if its value is 0, like the
 * following:
 * { $addFields:
 *     { <INTERNAL_FIELDS>.<inputPipelineName_rank>:
 *         { $cond: [
 *              {
 *                  $eq : [
 *                      "$<INTERNAL_FIELDS>.<inputPipelineName>_rank",
 *                      {
 *                          $const: 0
 *                      }
 *                  ]
 *              },
 *              {
 *                  $const: "NA"
 *              },
 *              "$<INTERNAL_FIELDS>.<inputPipelineName>_rank"
 *          ] },
 *     }
 * }
 * This is done, because, conceptually, if a rank has a value of 0, then that means the document was
 * not output from that input pipeline. So leaving its value as 0 would confuse the user in the
 * scoreDetails output since the lower the rank, the higher the relevance of the document. Thus,
 * this stage changes the value of the rank field to "NA" when applicable.
 */
boost::intrusive_ptr<DocumentSource> buildRankAddFieldsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<std::string>& pipelineNames) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        for (const auto& pipelineName : pipelineNames) {
            const std::string internalFieldsInputPipelineRankName =
                hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                    RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
                    fmt::format("{}_rank", pipelineName));
            const std::string rankPath = fmt::format("${}", internalFieldsInputPipelineRankName);
            addFieldsBob.append(internalFieldsInputPipelineRankName,
                                BSON("$cond" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY(rankPath << 0))
                                                           << BSON("$const" << "NA") << rankPath)));
        }
        addFieldsBob.done();
    }

    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Sets the score metadata to the value of the sum of all the added scores.
 * Ex:
 *  {
        "$setMetadata": {
            "score": {
                "$add": [
                    "$<INTERNAL_FIELDS>.<inputPipelineName1>_score",
                    "$<INTERNAL_FIELDS>.<inputPipelineName2>_score"
                ]
            }
        }
    },
 */
boost::intrusive_ptr<DocumentSource> calculateFinalScoreMetadata(
    const auto& expCtx, const std::vector<std::string>& pipelineNames) {
    // Generate an array of all the fields containing a score for a given pipeline.
    Expression::ExpressionVector allInputScores;
    for (const auto& pipelineName : pipelineNames) {
        allInputScores.push_back(ExpressionFieldPath::createPathFromString(
            expCtx.get(),
            RankFusionPipelineBuilder::kRankFusionInternalFieldsName + "." + pipelineName +
                "_score",
            expCtx->variablesParseState));
    }

    // Return a $setMetadata stage that sets score to an $add expression that takes the generated
    // array of each pipeline's score fieldpaths as an input. Ex: { "$setMetadata": { "score":
    // { "$add": ["$pipeline_name_score"] } } },
    return DocumentSourceSetMetadata::create(
        expCtx,
        make_intrusive<ExpressionAdd>(expCtx.get(), std::move(allInputScores)),
        DocumentMetadataFields::MetaType::kScore);
}

/**
 * Adds a field called "score" set to the value of the sum of all the added scores. This is used
 instead of setting the score metadata when the rankFusionFeatureFlag is off.
 * Ex:
 *  {
        "$addFields": {
            "score": {
                "$add": [
                    "$<INTERNAL_FIELDS>.<inputPipelineName1>_score",
                    "$<INTERNAL_FIELDS>.<inputPipelineName2>_score"
                ]
            }
        }
    },
 */
BSONObj calculateFinalScore(const std::vector<std::string>& pipelineNames) {
    // Generate a $add object with an array of all the fields containing a score for a given
    // pipeline.
    const auto& allInputs = [&] {
        BSONObjBuilder addBob;
        {
            BSONArrayBuilder addArrBuilder(addBob.subarrayStart("$add"_sd));
            for (const auto& pipelineName : pipelineNames) {
                StringBuilder sb;
                sb << "$" << RankFusionPipelineBuilder::kRankFusionInternalFieldsName << "."
                   << pipelineName << "_score";
                addArrBuilder.append(sb.str());
            }
            addArrBuilder.done();
        }
        return addBob.obj();
    };
    return BSON("$addFields" << BSON("score" << allInputs()));
}

/**
 * Constuct the scoreDetails metadata object. Looks like the following:
 * { "$setMetadata": { "scoreDetails": { "value": { $meta: "score" }, "description":
 * { "scoreDetailsDescription..." }, "details": "$calculatedScoreDetails" } } },
 */
boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const StringData scoreDetailsDescription,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    boost::intrusive_ptr<DocumentSource> setScoreDetails = DocumentSourceSetMetadata::create(
        expCtx,
        Expression::parseObject(
            expCtx.get(),
            BSON(
                "value" << BSON("$meta" << "score") << "description" << scoreDetailsDescription
                        << "details"
                        << fmt::format("${}",
                                       hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                           RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
                                           "calculatedScoreDetails"))),
            expCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}

/**
 * Append logic for applying scoreNulls behavior for the "rank" field.
 */
void RankFusionPipelineBuilder::groupDocsByIdAcrossInputPipelineScoreDetails(
    StringData pipelineName, BSONObjBuilder& pushBob) {
    const std::string rankName = fmt::format("{}_rank", pipelineName);
    pushBob.append(rankName,
                   BSON("$ifNull" << BSON_ARRAY(
                            fmt::format("${}",
                                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                            getInternalFieldsName(), rankName))
                            << 0)));
}

/**
 * Append logic for determining the max value of the "rank" field across documents.
 */
void RankFusionPipelineBuilder::projectReduceInternalFieldsScoreDetails(
    BSONObjBuilder& bob, StringData pipelineName, const bool forInitialValue) {
    if (forInitialValue) {
        bob.append(fmt::format("{}_rank", pipelineName), 0);
    } else {
        bob.append(fmt::format("{}_rank", pipelineName),
                   BSON("$max" << BSON_ARRAY(fmt::format("$$value.{}_rank", pipelineName)
                                             << fmt::format("$$this.{}_rank", pipelineName))));
    }
}

/**
 * Append logic for the $rankFusion-specific input pipeline scoreDetails values (rank and weight).
 */
void RankFusionPipelineBuilder::constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
    BSONObjBuilder& bob, StringData pipelineName, double weight) {
    std::string internalFieldsInputPipelineRankPath = fmt::format("${}_rank", pipelineName);
    bob.append("rank"_sd, internalFieldsInputPipelineRankPath);
    // In the scoreDetails output, for any input pipeline that didn't output
    // a document in the result, the default "rank" will be "NA" and the
    // weight will be omitted to make it clear to the user that the final
    // score for that document result did not take into account its input
    // pipeline's rank/weight.
    bob.append("weight",
               BSON("$cond" << BSON_ARRAY(
                        BSON("$eq" << BSON_ARRAY(internalFieldsInputPipelineRankPath << "NA"))
                        << "$$REMOVE" << weight)));
}

/**
 * Build stages for beginning of input pipeline.
 * { ... stages of input pipeline ... }
 * { "$replaceRoot": { "newRoot": { "<INTERNAL_DOCS>.": "$$ROOT" } } },
 * { "$_internalSetWindowFields": { "sortBy": { "order": 1 }, "output": {
 * "$<INTERNAL_FIELDS>.<inputPipelineName>_rank": { "$rank": {} } } } },
 * {
    "$addFields": {
        "<INTERNAL_FIELDS>.<inputPipelineName>_score": {
            "$multiply": [
                {
                    $divide: [
                        1,
                        { $add: [rank, rankConstant] }
                    ]
                },
                { "$const": 5.0 }
            ]
        }
    }
 * } // where 5.0 is the weight
 *
 * If scoreDetails is true, include the following stages:
 * { $addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: ...} }
 */
std::list<boost::intrusive_ptr<DocumentSource>>
RankFusionPipelineBuilder::buildInputPipelineDesugaringStages(
    StringData firstInputPipelineName,
    double weight,
    const std::unique_ptr<Pipeline>& pipeline,
    bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    makeSureSortKeyIsOutput(pipeline->getSources());
    const bool inputGeneratesScore =
        pipeline->generatesMetadataType(DocumentMetadataFields::kScore);
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    for (auto&& stage : pipeline->getSources()) {
        outputStages.emplace_back(stage);
    }

    outputStages.emplace_back(buildReplaceRootStage(expCtx));
    outputStages.emplace_back(
        setWindowFields(expCtx,
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            RankFusionPipelineBuilder::kRankFusionInternalFieldsName,
                            fmt::format("{}_rank", firstInputPipelineName))));
    outputStages.emplace_back(
        buildScoreAddFieldsStage(expCtx, firstInputPipelineName, getRankConstant(), weight));

    if (shouldIncludeScoreDetails()) {
        outputStages.emplace_back(addInputPipelineScoreDetails(
            expCtx, firstInputPipelineName, inputGeneratesScore, inputGeneratesScoreDetails));
    }
    return outputStages;
}

/**
 * Calculate the final score field to add to each document and sorts the documents by score and id,
 * and removes all internal processing fields.

 * The $sort stage looks like this: { "$sort": { "score": { $meta: "score" }, "_id": 1 } } for
 * rankFusionFull feature flag and like this { "$sort": { "score": -1, "_id": 1 } } for
 * rankFusionBasic feature flag.
 *
 * When scoreDetails is enabled, the $score metadata will be set after the grouping behavior
 * described above, then the final scoreDetails object will be calculated, the $scoreDetails
 * metadata will be set, and then the $sort and final exclusion $project stages will follow.
 */
std::list<boost::intrusive_ptr<DocumentSource>> RankFusionPipelineBuilder::buildScoreAndMergeStages(
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;

    // Override ranks with the value of 0.
    scoreAndMergeStages.emplace_back(buildRankAddFieldsStage(expCtx, pipelineNames));

    // Remove the internal fields object.
    auto removeInternalFieldsProject = DocumentSourceProject::createFromBson(
        projectRemoveInternalFieldsObject().firstElement(), expCtx);

    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (isRankFusionFullEnabled()) {
        // Set the final score.
        auto setScore = calculateFinalScoreMetadata(expCtx, pipelineNames);
        const SortPattern sortingPatternScoreMetadata{
            BSON("score" << BSON("$meta" << "score") << "_id" << 1), expCtx};
        boost::intrusive_ptr<DocumentSourceSort> sortScoreMetadata =
            DocumentSourceSort::create(expCtx, sortingPatternScoreMetadata);
        if (shouldIncludeScoreDetails()) {
            boost::intrusive_ptr<DocumentSource> addFieldsScoreDetails =
                constructCalculatedFinalScoreDetails(pipelineNames, weights, expCtx);
            auto setScoreDetails =
                constructScoreDetailsMetadata(getScoreDetailsDescription(), expCtx);
            scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                                       {std::move(setScore),
                                        std::move(addFieldsScoreDetails),
                                        std::move(setScoreDetails),
                                        std::move(sortScoreMetadata),
                                        std::move(removeInternalFieldsProject)});
            return scoreAndMergeStages;
        }
        scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                                   {std::move(setScore),
                                    std::move(sortScoreMetadata),
                                    std::move(removeInternalFieldsProject)});
        return scoreAndMergeStages;
    }

    auto addFields = DocumentSourceAddFields::createFromBson(
        calculateFinalScore(pipelineNames).firstElement(), expCtx);
    const SortPattern sortingPattern{BSON("score" << -1 << "_id" << 1), expCtx};
    boost::intrusive_ptr<DocumentSourceSort> sort =
        DocumentSourceSort::create(expCtx, sortingPattern);
    scoreAndMergeStages.splice(
        scoreAndMergeStages.end(),
        {std::move(addFields), std::move(sort), std::move(removeInternalFieldsProject)});
    return scoreAndMergeStages;
}
}  // namespace mongo
