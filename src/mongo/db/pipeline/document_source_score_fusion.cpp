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

#include "mongo/db/pipeline/document_source_score_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(scoreFusion,
                                           DocumentSourceScoreFusion::LiteParsed::parse,
                                           DocumentSourceScoreFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagSearchHybridScoringFull);
namespace {

// The ScoreFusionScoringOptions class validates and stores the normalization,
// combination.method, and combination.expression fields. combination.expression is not
// immediately parsed into an expression because any pipelines variables it references will be
// considered undefined and will therefore throw an error at parsing time.
// combination.expression will only be parsed into an expression when the enclosing $let var
// (which defines the pipeline variables) is constructed.
class ScoreFusionScoringOptions {
public:
    ScoreFusionScoringOptions(const ScoreFusionSpec& spec) {
        _normalizationMethod = spec.getInput().getNormalization();
        auto& combination = spec.getCombination();
        // The default combination method is avg if no combination method is specified.
        ScoreFusionCombinationMethodEnum combinationMethod = ScoreFusionCombinationMethodEnum::kAvg;
        boost::optional<IDLAnyType> combinationExpression = boost::none;
        if (combination.has_value() && combination->getMethod().has_value()) {
            combinationMethod = combination->getMethod().get();
            uassert(10017300,
                    "combination.expression should only be specified when combination.method "
                    "has the value \"expression\"",
                    (combinationMethod != ScoreFusionCombinationMethodEnum::kExpression &&
                     !combination->getExpression().has_value()) ||
                        (combinationMethod == ScoreFusionCombinationMethodEnum::kExpression &&
                         combination->getExpression().has_value()));
            combinationExpression = combination->getExpression();
            uassert(10017301,
                    "both combination.expression and combination.weights cannot be specified",
                    !(combination->getWeights().has_value() && combinationExpression.has_value()));
        }
        _combinationMethod = std::move(combinationMethod);
        _combinationExpression = std::move(combinationExpression);
    }

    ScoreFusionNormalizationEnum getNormalizationMethod() const {
        return _normalizationMethod;
    }

    std::string getNormalizationString(ScoreFusionNormalizationEnum normalization) const {
        switch (normalization) {
            case ScoreFusionNormalizationEnum::kSigmoid:
                return "sigmoid";
            case ScoreFusionNormalizationEnum::kMinMaxScaler:
                return "minMaxScaler";
            case ScoreFusionNormalizationEnum::kNone:
                return "none";
            default:
                // Only one of the above options can be specified for normalization.
                MONGO_UNREACHABLE_TASSERT(9467100);
        }
    }

    ScoreFusionCombinationMethodEnum getCombinationMethod() const {
        return _combinationMethod;
    }

    std::string getCombinationMethodString(ScoreFusionCombinationMethodEnum comboMethod) const {
        switch (comboMethod) {
            case ScoreFusionCombinationMethodEnum::kExpression:
                return "custom expression";
            case ScoreFusionCombinationMethodEnum::kAvg:
                return "average";
            default:
                // Only one of the above options can be specified for combination.method.
                MONGO_UNREACHABLE_TASSERT(9467101);
        }
    }

    boost::optional<IDLAnyType> getCombinationExpression() const {
        return _combinationExpression;
    }

private:
    // The default normalization value is ScoreFusionCombinationMethodEnum::kNone. The IDL
    // handles the default behavior.
    ScoreFusionNormalizationEnum _normalizationMethod;
    // The default combination.method value is ScoreFusionCombinationMethodEnum::kAvg. The IDL
    // handles the default behavior.
    ScoreFusionCombinationMethodEnum _combinationMethod;
    // This field should only be populated when combination.method has the value
    // ScoreFusionCombinationMethodEnum::kExpression.
    boost::optional<IDLAnyType> _combinationExpression = boost::none;
};

// Description that gets set as part of $scoreFusion's scoreDetails metadata.
static const std::string scoreFusionScoreDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across "
    "input pipelines from which this document is output from:";

// Stage name without the '$' prefix
static const std::string scoreFusionStageName = "scoreFusion";

// Below are helper functions that return stages or stage lists that represent sub-components
// of the total $scoreFusion desugar. They are defined in an order close to how
// they appear in the desugar read from top to bottom.
// Generally, during the intermediate processing of $scoreFusion, the docs moving through
// the pipeline look like:
// {
//   "_id": ...,
//   "<INTERNAL_FIELDS_DOCS>": { <unmodified document from collection> },
//   "<INTERNAL_FIELDS>"; { <internal variable for intermediate processing > }
// }

/**
 * Builds and returns an $addFields stage that computes the weighted and normalized score
 * of this input pipeline, assuming the incoming pipeline raw score is available in
 * {"$meta": "score"}. The computed score is stored in the field
 * "<INTERNAL_FIELDS>.<inputPipelineName>_score".
 *
 * Note if the normalization is $minMaxScaler, this stage only computes the pipeline weighting
 * and normalization is handled after with a $setWindowFields.
 *
 * Example:
 * {$addFields:
 *     {<INTERNAL_FIELDS>.<inputPipelineName>_score:
 *         {$multiply:
 *             [{"$score"}, 0.5] // or [{$meta: "vectorSearchScore"}, 0.5]
 *         },
 *     }
 * }
 */
boost::intrusive_ptr<DocumentSource> buildScoreAddFieldsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelineName,
    const ScoreFusionNormalizationEnum normalization,
    const double weight) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            const std::string internalFieldsInputPipelineScoreName =
                hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                    DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
                    fmt::format("{}_score", inputPipelineName));
            BSONObjBuilder scoreField(
                addFieldsBob.subobjStart(internalFieldsInputPipelineScoreName));
            {
                BSONObj scorePath = BSON("$meta" << "score");
                BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
                BSONObj normalizationScorePath;
                switch (normalization) {
                    case ScoreFusionNormalizationEnum::kSigmoid:
                        normalizationScorePath = BSON("$sigmoid" << scorePath);
                        break;
                    case ScoreFusionNormalizationEnum::kMinMaxScaler:
                        // For minMaxScaler normalization, parse just the score operator into
                        // the $addFields stage. The normalization will happen separately in a
                        // $setWindowFields stage, after the $addFields stage.
                    case ScoreFusionNormalizationEnum::kNone:
                        // In the case of no normalization, parse just the score operator
                        // itself.
                        normalizationScorePath = std::move(scorePath);
                        break;
                }
                multiplyArray.append(normalizationScorePath);
                multiplyArray.append(weight);
            }
        }
    }
    const BSONObj spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns an $addFields stage. Here, rawScore refers to the incoming score from the
 * input pipeline prior to any normalization or weighting:
 * {$addFields:
 *     {<INTERNAL_FIELDS>.<inputPipelineName>_rawScore:
 *         {
 *              "$meta": "score"
 *         }
 *     }
 * }
 */
boost::intrusive_ptr<DocumentSource> buildRawScoreAddFieldsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const StringData inputPipelineName) {
    BSONObjBuilder bob;
    {
        const std::string internalFieldsInputPipelineRawScoreName =
            hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
                fmt::format("{}_rawScore", inputPipelineName));
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        addFieldsBob.append(internalFieldsInputPipelineRawScoreName, BSON("$meta" << "score"));
    }
    const BSONObj spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}


/**
 * Builds and returns an $addFields stage that materializes scoreDetails for an individual input
 * pipeline. The way we materialize scoreDetails depends on if the input pipeline generates
 * "scoreDetails" metadata.
 *
 * Later, these individual input pipeline scoreDetails will be gathered together in order to build
 * scoreDetails for the overall $scoreFusion pipeline (see calculateFinalScoreDetails()).
 */
boost::intrusive_ptr<DocumentSource> addInputPipelineScoreDetails(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelinePrefix,
    const bool inputGeneratesScoreDetails) {
    const std::string scoreDetails = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
        DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
        fmt::format("{}_scoreDetails", inputPipelinePrefix));
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));

        if (inputGeneratesScoreDetails) {
            // If the input pipeline generates scoreDetails (for example, $search may generate
            // searchScoreDetails), then we'll use the existing details:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: details: {$meta:
            // "scoreDetails"}}}}
            addFieldsBob.append(scoreDetails, BSON("details" << BSON("$meta" << "scoreDetails")));
        } else {
            // All $scoreFusion input pipelines must be scored (generate a score).

            // Build our own scoreDetails for the pipeline like:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: {details: []}}}
            addFieldsBob.append(scoreDetails, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Adds the following stages for scoreDetails:
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_rawScore: { "$meta": "score" } } }
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: ...} }. See addScoreDetails'
 * comment for what the possible values for <inputPipelineName>_scoreDetails are.
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildInputPipelineScoreDetails(
    const StringData inputPipelineName,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    boost::intrusive_ptr<DocumentSource> rawScoreAddFields =
        buildRawScoreAddFieldsStage(expCtx, inputPipelineName);
    boost::intrusive_ptr<DocumentSource> scoreDetailsAddFields =
        addInputPipelineScoreDetails(expCtx, inputPipelineName, inputGeneratesScoreDetails);
    std::list<boost::intrusive_ptr<DocumentSource>> initialScoreDetails = {
        std::move(rawScoreAddFields), std::move(scoreDetailsAddFields)};
    return initialScoreDetails;
}

/**
 * Builds and returns a $setWindowFields stage, like the following:
 * {$setWindowFields:
 *     {sortBy:
 *         {<INTERNAL_FIELDS>.<pipeline_name>_score: -1
 *         },
 *      output:
 *          {<INTERNAL_FIELDS>.<pipeline_name>_score:
 *              {$minMaxScaler:
 *                  {input: "$<INTERNAL_FIELDS>.<pipeline_name>_score"
 *                  }
 *              }
 *          }
 *      }
 * }
 *
 * Unlike $sigmoid normalization, which only relies on value of the raw score to compute the
 * normalized score, $minMaxScaler needs to observe all the raw scores in each input pipeline to
 * produce each normalized score in that input pipeline. Thus this $setWindowFields stage is
 * appended once per input pipeline (both the first one, and each other one wrapped in the
 * $unionWith
 */
boost::intrusive_ptr<DocumentSource> builtSetWindowFieldsStageForMinMaxScalerNormalization(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const StringData inputPipelineName) {
    const std::string internalFieldsScore =
        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
            hybrid_scoring_util::getScoreFieldFromPipelineName(inputPipelineName));
    const std::string dollarScore = "$" + internalFieldsScore;
    SortPattern sortPattern{BSON(internalFieldsScore << -1), expCtx};

    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        boost::none,  // partitionBy
        sortPattern,
        std::vector<WindowFunctionStatement>{WindowFunctionStatement{
            internalFieldsScore,  // output field
            window_function::Expression::parse(
                BSON("$minMaxScaler" << BSON("input" << dollarScore)), sortPattern, expCtx.get())}},
        SbeCompatibility::notCompatible);
}

/**
 * Build stages for first pipeline. Example where the first pipeline is called "name1" and has a
 * weight of 5.0:
 * { ... stages of first pipeline ... }
 * { "$replaceRoot": { "newRoot": { "<INTERNAL_DOCS>.": "$$ROOT" } } },
 * { "$addFields": { "<INTERNAL_FIELDS>.name1_score": { "$multiply": [ { $meta: "score" }, {
 * "$const": 5.0 } ] } } }
 * If scoreDetails is true, include the following stages:
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_rawScore: { "$meta": "score" } } }
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: ...} }
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const StringData inputPipelineOneName,
    const ScoreFusionNormalizationEnum normalization,
    const double weight,
    const std::unique_ptr<Pipeline>& firstInputPipeline,
    const bool includeScoreDetails,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    while (!firstInputPipeline->empty()) {
        // These stages are being copied over from the original pipeline.
        outputStages.push_back(firstInputPipeline->popFront());
    }

    outputStages.emplace_back(hybrid_scoring_util::buildReplaceRootStage(
        DocumentSourceScoreFusion::kScoreFusionDocsFieldName, expCtx));
    outputStages.emplace_back(
        buildScoreAddFieldsStage(expCtx, inputPipelineOneName, normalization, weight));

    // TODO SERVER-105867: Investigate why these two stages have to happen on the shard and not on
    // the merging node in order for $score's scoreDetails to be populated correctly.
    if (includeScoreDetails) {
        std::list<boost::intrusive_ptr<DocumentSource>> initialScoreDetailsStages =
            buildInputPipelineScoreDetails(
                inputPipelineOneName, inputGeneratesScoreDetails, expCtx);
        outputStages.splice(outputStages.end(), std::move(initialScoreDetailsStages));
    }

    // Build the $setWindowFields stage, to perform minMaxScaler normalization, if applicable.
    if (normalization == ScoreFusionNormalizationEnum::kMinMaxScaler) {
        outputStages.emplace_back(
            builtSetWindowFieldsStageForMinMaxScalerNormalization(expCtx, inputPipelineOneName));
    }
    return outputStages;
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
 * If $scoreFusion's normalization is "minMaxScaler," then the pipeline stages under the
 * $unionWith.pipeline field will also contain an $_internalSetWindowFields stage that calculates
 * the minMaxScaler value of the pipeline's score.
 */
boost::intrusive_ptr<DocumentSource> buildUnionWithPipelineStage(
    const StringData inputPipelineName,
    const ScoreFusionNormalizationEnum normalization,
    const double weight,
    const std::unique_ptr<Pipeline>& oneInputPipeline,
    const bool includeScoreDetails,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    oneInputPipeline->pushBack(hybrid_scoring_util::buildReplaceRootStage(
        DocumentSourceScoreFusion::kScoreFusionDocsFieldName, expCtx));
    oneInputPipeline->pushBack(
        buildScoreAddFieldsStage(expCtx, inputPipelineName, normalization, weight));
    if (includeScoreDetails) {
        std::list<boost::intrusive_ptr<DocumentSource>> initialScoreDetailsStages =
            buildInputPipelineScoreDetails(inputPipelineName, inputGeneratesScoreDetails, expCtx);
        for (auto&& docSource : initialScoreDetailsStages) {
            oneInputPipeline->pushBack(docSource);
        }
    }
    // Build the $setWindowFields stage, to perform minMaxScaler normalization, if applicable.
    if (normalization == ScoreFusionNormalizationEnum::kMinMaxScaler) {
        oneInputPipeline->pushBack(
            builtSetWindowFieldsStageForMinMaxScalerNormalization(expCtx, inputPipelineName));
    }

    std::vector<BSONObj> bsonPipeline = oneInputPipeline->serializeToBson();

    auto collName = expCtx->getUserNss().coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

/**
 * Calculate the final score by combining the score fields on each input document according to the
 * $scoreFusion specification and adding it as a new field to the document.
 * { "$setMetadata": { "score": { "$avg": [ "$<INTERNAL_FIELDS>.name1_score",
 * "<INTERNAL_FIELDS>.$name2_score" ] } } }
 */
boost::intrusive_ptr<DocumentSource> buildSetFinalCombinedScoreStage(
    const auto& expCtx,
    const std::vector<std::string>& pipelineNames,
    const ScoreFusionScoringOptions scoreFusionScoringOptions) {
    ScoreFusionCombinationMethodEnum combinationMethod =
        scoreFusionScoringOptions.getCombinationMethod();
    // Default is to average the scores.
    boost::intrusive_ptr<Expression> metadataExpression;
    switch (combinationMethod) {
        case ScoreFusionCombinationMethodEnum::kExpression: {
            boost::optional<IDLAnyType> combinationExpression =
                scoreFusionScoringOptions.getCombinationExpression();
            // Earlier logic checked that combination.expression's value must be present if
            // combination.method has the value 'expression.'

            // Assemble $let.vars field. It is a BSON obj of pipeline names to their corresponding
            // pipeline score field. Ex: {geo_doc: "$geo_doc_score"}.
            BSONObjBuilder varsAndInFields;
            for (const auto& pipelineName : pipelineNames) {
                std::string fieldScoreName = hybrid_scoring_util::getScoreFieldFromPipelineName(
                    hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                        DocumentSourceScoreFusion::kScoreFusionInternalFieldsName, pipelineName),
                    true);
                varsAndInFields.appendElements(BSON(pipelineName << fieldScoreName));
            }
            varsAndInFields.done();

            // Assemble $let expression. For example: { "$let": { "vars": { "geo_doc":
            // "$geo_doc_score" }, "in": { "$sum": ["$$geo_doc", 5.0] } } },
            // where the user-inputted combination.expression is: { "$sum": ["$$geo_doc", 5.0] }
            // This is done so the user-inputted pipeline name variables correctly evaluate to each
            // pipeline's underlying score field path. Ex: pipeline name $$geo_doc maps to
            // $geo_doc_score.

            // At this point, we can't be sure that the user-provided expression evaluates to a
            // numeric type. However, upon attempting to set the metadata score field with this
            // expression, if it does not evaluate to a numeric type, then we will throw a
            // TypeMismatch error.
            metadataExpression = ExpressionLet::parse(
                expCtx.get(),
                BSON("$let" << BSON("vars" << varsAndInFields.obj() << "in"
                                           << combinationExpression->getElement()))
                    .firstElement(),
                expCtx->variablesParseState);
            break;
        }
        case ScoreFusionCombinationMethodEnum::kAvg: {
            // Construct an array of the score field path names for AccumulatorAvg.
            BSONArrayBuilder expressionFieldPaths;
            for (const auto& pipelineName : pipelineNames) {
                std::string fieldScoreName = hybrid_scoring_util::getScoreFieldFromPipelineName(
                    hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                        DocumentSourceScoreFusion::kScoreFusionInternalFieldsName, pipelineName),
                    true);
                expressionFieldPaths.append(fieldScoreName);
            }
            expressionFieldPaths.done();
            metadataExpression = ExpressionFromAccumulator<AccumulatorAvg>::parse(
                expCtx.get(),
                BSON("$avg" << expressionFieldPaths.arr()).firstElement(),
                expCtx->variablesParseState);
            break;
        }
        default:
            // Only one of the above options can be specified for combination.method.
            MONGO_UNREACHABLE_TASSERT(10016700);
    }
    return DocumentSourceSetMetadata::create(
        expCtx, metadataExpression, DocumentMetadataFields::MetaType::kScore);
}

/**
 * Construct the final scoreDetails metadata object (this metadata contains the end product of
 * normalization and combination and is what the user sees as the final output of $scoreFusion).
 * Looks like the following:
 * { "$setMetadata":
 *  { "scoreDetails":
 *     { "value": "$score",
 *       "description": {"scoreDetailsDescription..."},
 *       "normalization": "norm",
 *       "combination": {"method": "combinationMethod"},
 *       details": "$calculatedScoreDetails"
 *     }
 *  }
 * },
 *
 * If combination.method is "expression" then the "combination" field above will look like this:
 * "combination": {"method": "custom expression", "expression": "stringified expression"}
 */
boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const ScoreFusionScoringOptions scoreFusionScoringOptions,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONObjBuilder combinationBob(
        BSON("method" << scoreFusionScoringOptions.getCombinationMethodString(
                 scoreFusionScoringOptions.getCombinationMethod())));
    if (scoreFusionScoringOptions.getCombinationMethod() ==
        ScoreFusionCombinationMethodEnum::kExpression) {
        combinationBob.append("expression",
                              hybrid_scoring_util::score_details::stringifyExpression(
                                  scoreFusionScoringOptions.getCombinationExpression()));
    }
    combinationBob.done();
    boost::intrusive_ptr<DocumentSource> setScoreDetails = DocumentSourceSetMetadata::create(
        expCtx,
        Expression::parseObject(
            expCtx.get(),
            BSON("value" << BSON("$meta" << "score") << "description"
                         << scoreFusionScoreDetailsDescription << "normalization"
                         << scoreFusionScoringOptions.getNormalizationString(
                                scoreFusionScoringOptions.getNormalizationMethod())
                         << "combination" << combinationBob.obj() << "details"
                         << "$" +
                     hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
                                "calculatedScoreDetails")),
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
    const ScoreFusionScoringOptions metadata,
    const StringMap<double>& weights,
    const bool includeScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;

    // Group all the documents across the different $unionWiths for each input pipeline.
    scoreAndMergeStages.emplace_back(DocumentSourceGroup::createFromBson(
        hybrid_scoring_util::groupDocsByIdAcrossInputPipeline(
            DocumentSourceScoreFusion::kScoreFusionDocsFieldName,
            DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
            pipelineNames,
            includeScoreDetails)
            .firstElement(),
        expCtx));

    // Combine all internal processing fields into one blob.
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectReduceInternalFields(
            DocumentSourceScoreFusion::kScoreFusionDocsFieldName,
            DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
            pipelineNames,
            includeScoreDetails)
            .firstElement(),
        expCtx));

    // Promote the user's documents back to the top-level so that we can evaluate the expression
    // potentially using fields from the user's documents.
    scoreAndMergeStages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        hybrid_scoring_util::promoteEmbeddedDocsObject(
            DocumentSourceScoreFusion::kScoreFusionDocsFieldName)
            .firstElement(),
        expCtx));
    scoreAndMergeStages.emplace_back(DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectRemoveEmbeddedDocsObject(
            DocumentSourceScoreFusion::kScoreFusionDocsFieldName)
            .firstElement(),
        expCtx));

    // Set the final score.
    scoreAndMergeStages.emplace_back(
        buildSetFinalCombinedScoreStage(expCtx, pipelineNames, metadata));

    // Note that the scoreDetails fields go here in the pipeline. We create them below to be
    // able to return them immediately once all stages are generated.
    const SortPattern sortingPattern{BSON("score" << BSON("$meta" << "score") << "_id" << 1),
                                     expCtx};
    auto sort = DocumentSourceSort::create(expCtx, sortingPattern);

    // Calculate score details, if necessary.
    if (includeScoreDetails) {
        auto addFieldsScoreDetails =
            hybrid_scoring_util::score_details::constructCalculatedFinalScoreDetails(
                DocumentSourceScoreFusion::kScoreFusionInternalFieldsName,
                pipelineNames,
                weights,
                expCtx);
        auto setScoreDetails = constructScoreDetailsMetadata(metadata, expCtx);
        scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                                   {std::move(addFieldsScoreDetails), std::move(setScoreDetails)});
    }

    // Remove the internal fields object.
    auto removeInternalFieldsProject = DocumentSourceProject::createFromBson(
        hybrid_scoring_util::projectRemoveInternalFieldsObject(
            DocumentSourceScoreFusion::kScoreFusionInternalFieldsName)
            .firstElement(),
        expCtx);
    scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                               {std::move(sort), std::move(removeInternalFieldsProject)});
    return scoreAndMergeStages;
}
}  // namespace

std::unique_ptr<DocumentSourceScoreFusion::LiteParsed> DocumentSourceScoreFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::object);

    auto parsedSpec = ScoreFusionSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));
    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Parse each pipeline.
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    std::transform(
        inputPipesObj.begin(),
        inputPipesObj.end(),
        std::back_inserter(liteParsedPipelines),
        [nss](const auto& elem) { return LiteParsedPipeline(nss, parsePipelineFromBSON(elem)); });

    return std::make_unique<DocumentSourceScoreFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}

/**
 * Checks that the input pipeline is a valid scored pipeline. This means it is either one of
 * $search, $vectorSearch, $scoreFusion, $rankFusion (which have scored output) or has an explicit
 * $score stage. A scored pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void scoreFusionBsonPipelineValidator(const std::vector<BSONObj>& pipeline,
                                             boost::intrusive_ptr<ExpressionContext> expCtx) {
    static const std::string scorePipelineMsg =
        "All subpipelines to the $scoreFusion stage must begin with one of $search, "
        "$vectorSearch, or have a custom $score in the pipeline.";
    uassert(9402503,
            str::stream() << "$scoreFusion input pipeline cannot be empty. " << scorePipelineMsg,
            !pipeline.empty());

    uassert(10473003,
            "$scoreFusion input pipeline has a nested hybrid search stage "
            "($rankFusion/$scoreFusion). " +
                scorePipelineMsg,
            !hybrid_scoring_util::isHybridSearchPipeline(pipeline));

    auto scoredPipelineStatus = hybrid_scoring_util::isScoredPipeline(pipeline, expCtx);
    if (!scoredPipelineStatus.isOK()) {
        uasserted(9402500, scorePipelineMsg + " " + scoredPipelineStatus.reason());
    }

    auto selectionPipelineStatus = hybrid_scoring_util::isSelectionPipeline(pipeline);
    if (!selectionPipelineStatus.isOK()) {
        uasserted(9402502,
                  selectionPipelineStatus.reason() +
                      " Only stages that retrieve, limit, or order documents are allowed.");
    }
}

static void scoreFusionPipelineValidator(const Pipeline& pipeline) {
    tassert(
        10535800,
        "The metadata dependency tracker determined $scoreFusion input pipeline does not generate "
        "score metadata, despite the input pipeline stages being previously validated as such.",
        pipeline.generatesMetadataType(DocumentMetadataFields::kScore));
}

/**
 * Validate that each pipeline is a valid scored selection pipeline. Returns a pair of the map of
 * the input pipeline names to pipeline objects and a map of pipeline names to score paths.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseAndValidateScoredSelectionPipelines(
    const ScoreFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    for (const auto& innerPipelineBsonElem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(innerPipelineBsonElem);
        // Ensure that all pipelines are valid scored selection pipelines.
        scoreFusionBsonPipelineValidator(bsonPipeline, pExpCtx);

        auto pipeline = Pipeline::parse(bsonPipeline, pExpCtx);
        scoreFusionPipelineValidator(*pipeline);

        // Validate pipeline name.
        auto inputName = innerPipelineBsonElem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$scoreFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(9402203,
                str::stream()
                    << "$scoreFusion pipeline names must be unique, but found duplicate name '"
                    << inputName << "'.",
                !inputPipelines.contains(inputName));

        // Input pipeline has been validated; save it in the resulting maps.
        inputPipelines[inputName] = std::move(pipeline);
    }
    return inputPipelines;
}

// To fully understand the structure of the desugared output returned from this function, you
// can read the desugared output in the CheckOnePipelineAllowed and
// CheckMultiplePipelinesAllowed test cases under document_source_score_fusion_test.cpp.
std::list<boost::intrusive_ptr<DocumentSource>> constructDesugaredOutput(
    const ScoreFusionSpec& spec,
    const std::map<std::string, std::unique_ptr<Pipeline>>& inputPipelines,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    StringMap<double> weights;
    // If ScoreFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value() && combinationSpec->getWeights().has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights()->getOwned(), inputPipelines, scoreFusionStageName);
    }
    ScoreFusionNormalizationEnum normalization = spec.getInput().getNormalization();
    const bool includeScoreDetails = spec.getScoreDetails();
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    // Array to store pipeline names separately because Pipeline objects in the inputPipelines map
    // will be moved eventually to other structures, rendering inputPipelines unusable. With this
    // array, we can safely use/pass the pipeline names information without using inputPipelines.
    // Note that pipeline names are stored in the same order in which pipelines are desugared.
    std::vector<std::string> pipelineNames;
    for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
         pipeline_it++) {
        const auto& [inputPipelineName, inputPipelineStages] = *pipeline_it;

        pipelineNames.push_back(inputPipelineName);

        // Check if an explicit weight for this pipeline has been specified.
        // If not, the default is one.
        double pipelineWeight = hybrid_scoring_util::getPipelineWeight(weights, inputPipelineName);

        const bool inputGeneratesScoreDetails =
            inputPipelineStages->generatesMetadataType(DocumentMetadataFields::kScoreDetails);

        if (pipeline_it == inputPipelines.begin()) {
            // Stages for the first pipeline.
            auto firstPipelineStages = buildFirstPipelineStages(inputPipelineName,
                                                                normalization,
                                                                pipelineWeight,
                                                                std::move(inputPipelineStages),
                                                                includeScoreDetails,
                                                                inputGeneratesScoreDetails,
                                                                pExpCtx);
            outputStages.splice(outputStages.end(), std::move(firstPipelineStages));
        } else {
            // For the input pipelines other than the first,
            // we wrap then in a $unionWith stage to append it to the total desugared output.
            auto unionWithStage = buildUnionWithPipelineStage(inputPipelineName,
                                                              normalization,
                                                              pipelineWeight,
                                                              std::move(inputPipelineStages),
                                                              includeScoreDetails,
                                                              inputGeneratesScoreDetails,
                                                              pExpCtx);
            outputStages.emplace_back(unionWithStage);
        }
    }

    // Build all remaining stages to perform the fusion.
    // The ScoreFusionScoringOptions class sets the combination.method and combination.expression to
    // the correct user input after performing the necessary error checks (ex: verify that if
    // combination.method is 'custom', then the combination.expression should've been specified).
    // Average is the default combination method if no other method is specified.
    ScoreFusionScoringOptions scoreFusionScoringOptions(spec);
    auto finalStages = buildScoreAndMergeStages(
        pipelineNames, scoreFusionScoringOptions, weights, includeScoreDetails, pExpCtx);
    outputStages.splice(outputStages.end(), std::move(finalStages));
    return outputStages;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = ScoreFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseAndValidateScoredSelectionPipelines(spec, pExpCtx);

    // It is currently necessary to annotate on the ExpressionContext that this is a $scoreFusion
    // query. Once desugaring happens, there's no way to identity from the (desugared) pipeline
    // alone that it came from $scoreFusion. We need to know if it came from $scoreFusion so we can
    // reject the query if it is run over a view.

    // This flag's value is also used to gate an internal client error. See
    // search_helper::validateViewNotSetByUser(...) for more details.
    pExpCtx->setIsHybridSearch();

    return constructDesugaredOutput(spec, std::move(inputPipelines), pExpCtx);
}
}  // namespace mongo
