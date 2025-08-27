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

#include "mongo/db/pipeline/document_source_rank_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <algorithm>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(rankFusion,
                                           DocumentSourceRankFusion::LiteParsed::parse,
                                           DocumentSourceRankFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagRankFusionBasic);

namespace {

// Description that gets set as part of $rankFusion's scoreDetails metadata.
static const std::string rankFusionScoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / "
    "(60 + rank))) across input pipelines from which this document is output, from:";

// Stage name without the '$' prefix
static const std::string rankFusionStageName = "rankFusion";

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
 * Checks that the input pipeline is a valid ranked pipeline. This means it is either one of
 * $search, $vectorSearch, $geoNear, $rankFusion, $scoreFusion (which have ordered output) or has an
 * explicit $sort stage. A ranked pipeline must also be a 'selection pipeline', which means no stage
 * can modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void rankFusionBsonPipelineValidator(const std::vector<BSONObj>& pipeline) {
    static const std::string rankPipelineMsg =
        "All subpipelines to the $rankFusion stage must begin with one of $search, "
        "$vectorSearch, $geoNear, or have a custom $sort in the pipeline.";
    uassert(9834300,
            str::stream() << "$rankFusion input pipeline cannot be empty. " << rankPipelineMsg,
            !pipeline.empty());

    uassert(
        10473002,
        "$rankFusion input pipeline has a nested hybrid search stage ($rankFusion/$scoreFusion). " +
            rankPipelineMsg,
        !hybrid_scoring_util::isHybridSearchPipeline(pipeline));

    uassert(10614800,
            "$rankFusion input pipelines must not contain a $score stage.",
            !hybrid_scoring_util::pipelineContainsScoreStage(pipeline));

    auto rankedPipelineStatus = hybrid_scoring_util::isRankedPipeline(pipeline);
    if (!rankedPipelineStatus.isOK()) {
        uasserted(9191100, rankedPipelineStatus.reason() + " " + rankPipelineMsg);
    }

    auto selectionPipelineStatus = hybrid_scoring_util::isSelectionPipeline(pipeline);
    if (!selectionPipelineStatus.isOK()) {
        uasserted(9191103,
                  selectionPipelineStatus.reason() +
                      " Only stages that retrieve, limit, or order documents are allowed.");
    }
}

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
 * {$addFields:
 *     {<INTERNAL_FIELDS>.<inputPipelineName>_score:
 *         {multiply:
 *             [{$divide: [1, {$add: [rank, rankConstant]}]}]
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
                    DocumentSourceRankFusion::kRankFusionInternalFieldsName,
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
                                        DocumentSourceRankFusion::kRankFusionInternalFieldsName,
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
 * Builds and returns an $addFields stage that sets the rank to "NA" if its value is 0, like the
 * following:
 * {$addFields:
 *     {<INTERNAL_FIELDS>.<inputPipelineName_rank>:
 *         {$cond: [
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
 *          ]},
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
                    DocumentSourceRankFusion::kRankFusionInternalFieldsName,
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
        DocumentSourceRankFusion::kRankFusionInternalFieldsName,
        fmt::format("{}_scoreDetails", inputPipelineName));
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));

        if (inputGeneratesScoreDetails) {
            // If the input pipeline generates scoreDetails (for example, $search may generate
            // searchScoreDetails), then we'll use the existing details:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: {$meta:
            // "scoreDetails"}}} We don't grab {$meta: "score"} because we assume any existing
            // scoreDetails already includes its own score at "scoreDetails.value".
            addFieldsBob.append(scoreDetails, BSON("$meta" << "scoreDetails"));
        } else if (inputGeneratesScore) {
            // If the input pipeline does not generate scoreDetails but does generate a "score" (for
            // example, a $text query sorted on the text score), we'll build our own scoreDetails
            // for the pipeline like:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: {value: {$meta:
            // "score"}, details: []}}}
            addFieldsBob.append(
                scoreDetails,
                BSON("value" << BSON("$meta" << "score") << "details" << BSONArrayBuilder().arr()));
        } else {
            // If the input pipeline generates neither "score" not "scoreDetails" (for example, a
            // pipeline with just a $sort), we don't have any interesting information to include in
            // scoreDetails (rank is added later). We'll still build empty scoreDetails to
            // reflect that:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: {details: []}}}
            addFieldsBob.append(scoreDetails, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Build stages for first pipeline.
 * { ... stages of first pipeline ... }
 * { "$replaceRoot": { "newRoot": { "<INTERNAL_DOCS>.": "$$ROOT" } } },
 * { "$_internalSetWindowFields": {"sortBy": { "order": 1}, "output": {
 * "$<INTERNAL_FIELDS>.<inputPipelineName>_rank": { "$rank": {} } } } },
 * { "$addFields": { "<INTERNAL_FIELDS>.<inputPipelineName>_score": { "$multiply": [ {$divide: [1,
 * {$add: [rank, rankConstant]}]}, {"$const": 5.0 } ] } } } // where 5.0 is the weight
 *
 * If scoreDetails is true, include the following stages:
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: ...} }
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const StringData firstInputPipelineName,
    const int rankConstant,
    const double weight,
    std::unique_ptr<Pipeline> pipeline,
    const bool includeScoreDetails,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    while (!pipeline->empty()) {
        outputStages.emplace_back(pipeline->popFront());
    }

    outputStages.emplace_back(hybrid_scoring_util::buildReplaceRootStage(
        DocumentSourceRankFusion::kRankFusionDocsFieldName, expCtx));
    outputStages.emplace_back(
        setWindowFields(expCtx,
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            DocumentSourceRankFusion::kRankFusionInternalFieldsName,
                            fmt::format("{}_rank", firstInputPipelineName))));
    outputStages.emplace_back(
        buildScoreAddFieldsStage(expCtx, firstInputPipelineName, rankConstant, weight));

    if (includeScoreDetails) {
        outputStages.push_back(addInputPipelineScoreDetails(
            expCtx, firstInputPipelineName, inputGeneratesScore, inputGeneratesScoreDetails));
    }
    return outputStages;
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
                sb << "$" << DocumentSourceRankFusion::kRankFusionInternalFieldsName << "."
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
            DocumentSourceRankFusion::kRankFusionInternalFieldsName + "." + pipelineName + "_score",
            expCtx->variablesParseState));
    }

    // Return a $setMetadata stage that sets score to an $add object that takes the generated array
    // of each pipeline's score fieldpaths as an input.
    // Ex: {"$setMetadata": {"score": {"$add": ["$pipeline_name_score"]}}},
    return DocumentSourceSetMetadata::create(
        expCtx,
        make_intrusive<ExpressionAdd>(expCtx.get(), std::move(allInputScores)),
        DocumentMetadataFields::MetaType::kScore);
}

/**
 * Build the pipeline input to $unionWith (consists of a $replaceRoot and $addFields stage). Returns
 * a $unionWith stage that looks something like this:
 * { "$unionWith": { "coll": "pipeline_test", "pipeline": [inputPipeline stage (ex: $vectorSearch),
 * $replaceRoot stage, $addFields stage (add field to track input pipeline's weighted score) ] } }
 *
 * If score details are enabled, then the pipeline stages under the $unionWith.pipeline field will
 * also contain an $addFields stage that tracks the pipeline's scoreDetails.
 *
 */
boost::intrusive_ptr<DocumentSource> buildUnionWithPipeline(
    const std::string& inputPipelineName,
    const int rankConstant,
    const double weight,
    std::unique_ptr<Pipeline> oneInputPipeline,
    const bool includeScoreDetails,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    makeSureSortKeyIsOutput(oneInputPipeline->getSources());
    oneInputPipeline->pushBack(hybrid_scoring_util::buildReplaceRootStage(
        DocumentSourceRankFusion::kRankFusionDocsFieldName, expCtx));
    oneInputPipeline->pushBack(
        setWindowFields(expCtx,
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            DocumentSourceRankFusion::kRankFusionInternalFieldsName,
                            fmt::format("{}_rank", inputPipelineName))));
    oneInputPipeline->pushBack(
        buildScoreAddFieldsStage(expCtx, inputPipelineName, rankConstant, weight));
    if (includeScoreDetails) {
        oneInputPipeline->pushBack(addInputPipelineScoreDetails(
            expCtx, inputPipelineName, inputGeneratesScore, inputGeneratesScoreDetails));
    }
    std::vector<BSONObj> bsonPipeline = oneInputPipeline->serializeToBson();

    auto collName = expCtx->getUserNss().coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

/**
 * Constuct the scoreDetails metadata object. Looks like the following:
 * { "$setMetadata": {"scoreDetails": {"value": {$meta: "score"}, "description":
 * {"scoreDetailsDescription..."}, "details": "$calculatedScoreDetails"}}},
 */
boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const std::string& scoreDetailsDescription,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    boost::intrusive_ptr<DocumentSource> setScoreDetails = DocumentSourceSetMetadata::create(
        expCtx,
        Expression::parseObject(
            expCtx.get(),
            BSON("value" << BSON("$meta" << "score") << "description" << scoreDetailsDescription
                         << "details"
                         << fmt::format("${}",
                                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                            DocumentSourceRankFusion::kRankFusionInternalFieldsName,
                                            "calculatedScoreDetails"))),
            expCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}

/**
 * After all the pipelines have been executed and unioned, builds the $group stage to merge the
 * scoreFields/apply score nulls behavior, calculate the final score field to add to each document,
 * sorts the documents by score and id, and removes all internal processing fields.

 * The $sort stage looks like this: { "$sort": { "score": {$meta: "score"}, "_id": 1 } }
 *
 * When scoreDetails is enabled, the $score metadata will be set after the grouping behavior
 * described above, then the final scoreDetails object will be calculated, the $scoreDetails
 * metadata will be set, and then the $sort and final exclusion $project stages will follow.
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const bool includeScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;

    // Group all the documents across the different $unionWiths for each input pipeline.
    auto group = DocumentSourceGroup::createFromBson(
        hybrid_scoring_util::groupDocsByIdAcrossInputPipeline(
            DocumentSourceRankFusion::kRankFusionDocsFieldName,
            DocumentSourceRankFusion::kRankFusionInternalFieldsName,
            pipelineNames,
            includeScoreDetails)
            .firstElement(),
        expCtx);
    scoreAndMergeStages.push_back(group);

    // Combine all internal processing fields into one blob.
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectReduceInternalFields(
            DocumentSourceRankFusion::kRankFusionDocsFieldName,
            DocumentSourceRankFusion::kRankFusionInternalFieldsName,
            pipelineNames,
            includeScoreDetails)
            .firstElement(),
        expCtx));

    // Promote the user's documents back to the top-level so that we can evaluate the expression
    // potentially using fields from the user's documents.
    scoreAndMergeStages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        hybrid_scoring_util::promoteEmbeddedDocsObject(
            DocumentSourceRankFusion::kRankFusionDocsFieldName)
            .firstElement(),
        expCtx));
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectRemoveEmbeddedDocsObject(
            DocumentSourceRankFusion::kRankFusionDocsFieldName)
            .firstElement(),
        expCtx));

    // Override ranks with the value of 0.
    scoreAndMergeStages.emplace_back(buildRankAddFieldsStage(expCtx, pipelineNames));

    // Remove the internal fields object.
    auto removeInternalFieldsProject = DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectRemoveInternalFieldsObject(
            DocumentSourceRankFusion::kRankFusionInternalFieldsName)
            .firstElement(),
        expCtx);

    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Set the final score.
        auto setScore = calculateFinalScoreMetadata(expCtx, pipelineNames);
        const SortPattern sortingPatternScoreMetadata{
            BSON("score" << BSON("$meta" << "score") << "_id" << 1), expCtx};
        boost::intrusive_ptr<DocumentSourceSort> sortScoreMetadata =
            DocumentSourceSort::create(expCtx, sortingPatternScoreMetadata);
        if (includeScoreDetails) {
            boost::intrusive_ptr<DocumentSource> addFieldsScoreDetails =
                hybrid_scoring_util::score_details::constructCalculatedFinalScoreDetails(
                    DocumentSourceRankFusion::kRankFusionInternalFieldsName,
                    pipelineNames,
                    weights,
                    expCtx);
            auto setScoreDetails =
                constructScoreDetailsMetadata(rankFusionScoreDetailsDescription, expCtx);
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

}  // namespace

std::unique_ptr<DocumentSourceRankFusion::LiteParsed> DocumentSourceRankFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::object);

    auto parsedSpec = RankFusionSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));
    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Ensure that all pipelines are valid ranked selection pipelines.
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    std::transform(
        inputPipesObj.begin(),
        inputPipesObj.end(),
        std::back_inserter(liteParsedPipelines),
        [nss](const auto& elem) { return LiteParsedPipeline(nss, parsePipelineFromBSON(elem)); });

    return std::make_unique<DocumentSourceRankFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}

/**
 * Validate that each pipeline is a valid ranked selection pipeline. Returns a pair of the map of
 * the input pipeline names to pipeline objects and a map of pipeline names to score paths.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseAndValidateRankedSelectionPipelines(
    const RankFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& elem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(elem);
        rankFusionBsonPipelineValidator(bsonPipeline);

        auto pipeline = Pipeline::parse(bsonPipeline, pExpCtx);

        auto inputName = elem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$rankFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(
            9921000,
            str::stream() << "$rankFusion pipeline names must be unique, but found duplicate name '"
                          << inputName << "'.",
            !inputPipelines.contains(inputName));
        inputPipelines[inputName] = std::move(pipeline);
    }
    return inputPipelines;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = RankFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    auto inputPipelines = parseAndValidateRankedSelectionPipelines(spec, pExpCtx);

    // It is currently necessary to annotate on the ExpressionContext that this is a $rankFusion
    // query. Once desugaring happens, there's no way to identity from the (desugared) pipeline
    // alone that it came from $rankFusion. We need to know if it came from $rankFusion so we can
    // reject the query if it is run over a view.

    // This flag's value is also used to gate an internal client error. See
    // search_helper::validateViewNotSetByUser(...) for more details.
    pExpCtx->setIsHybridSearch();

    StringMap<double> weights;
    // If RankFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, rankFusionStageName);
    }

    // For now, the rankConstant is always 60.
    static const double rankConstant = 60;
    const bool includeScoreDetails = spec.getScoreDetails();
    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (includeScoreDetails) {
        auto isRankFusionFullEnabled =
            feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                "'featureFlagRankFusionFull' must be enabled to use scoreDetails",
                isRankFusionFullEnabled);
    }

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    // Array to store pipeline names separately because Pipeline objects in the inputPipelines map
    // will be moved eventually to other structures, rendering inputPipelines unusable. With this
    // array, we can safely use/pass the pipeline names information without using inputPipelines.
    // Note that pipeline names are stored in the same order in which pipelines are desugared.
    std::vector<std::string> pipelineNames;
    for (auto it = inputPipelines.begin(); it != inputPipelines.end(); it++) {
        const auto& name = it->first;
        auto& pipeline = it->second;

        pipelineNames.push_back(name);

        // Check if an explicit weight for this pipeline has been specified.
        // If not, the default is one.
        double pipelineWeight = hybrid_scoring_util::getPipelineWeight(weights, name);

        // We need to know if the pipeline generates "score" and "scoreDetails" metadata so we know
        // how to construct each pipeline's individual "scoreDetails" (see addScoreDetails()).
        const bool inputGeneratesScore =
            pipeline->generatesMetadataType(DocumentMetadataFields::kScore);
        const bool inputGeneratesScoreDetails =
            pipeline->generatesMetadataType(DocumentMetadataFields::kScoreDetails);

        if (outputStages.empty()) {
            // First pipeline.
            makeSureSortKeyIsOutput(pipeline->getSources());

            auto firstPipelineStages = buildFirstPipelineStages(name,
                                                                rankConstant,
                                                                pipelineWeight,
                                                                std::move(pipeline),
                                                                includeScoreDetails,
                                                                inputGeneratesScore,
                                                                inputGeneratesScoreDetails,
                                                                pExpCtx);
            outputStages.splice(outputStages.end(), std::move(firstPipelineStages));
        } else {
            auto unionWithStage = buildUnionWithPipeline(name,
                                                         rankConstant,
                                                         pipelineWeight,
                                                         std::move(pipeline),
                                                         includeScoreDetails,
                                                         inputGeneratesScore,
                                                         inputGeneratesScoreDetails,
                                                         pExpCtx);
            outputStages.push_back(unionWithStage);
        }
    }

    // Build all remaining stages to perform the fusion.
    auto finalStages =
        buildScoreAndMergeStages(pipelineNames, weights, includeScoreDetails, pExpCtx);
    outputStages.splice(outputStages.end(), std::move(finalStages));

    return outputStages;
}
}  // namespace mongo
