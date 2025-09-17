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

#include "mongo/db/pipeline/score_fusion_pipeline_builder.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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
 * Note that if the normalization is $minMaxScaler, this stage only computes the pipeline weighting
 * and normalization is handled after via a $setWindowFields.
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
                    ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName,
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
                ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName,
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
        ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName,
        fmt::format("{}_scoreDetails", inputPipelinePrefix));
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));

        if (inputGeneratesScoreDetails) {
            // If the input pipeline generates scoreDetails (for example, $search may generate
            // searchScoreDetails), then we'll use the existing details:
            // {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: details: {$meta:
            // "scoreDetails"}}}} We don't grab {$meta: "score"} because we assume any existing
            // scoreDetails already includes its own score at "scoreDetails.value".
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
 * appended once per input pipeline (both the first one, and each subsequent pipeline wrapped in the
 * $unionWith).
 */
boost::intrusive_ptr<DocumentSource> builtSetWindowFieldsStageForMinMaxScalerNormalization(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const StringData inputPipelineName) {
    const std::string internalFieldsScore =
        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName,
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
                        ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName, pipelineName),
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
                        ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName, pipelineName),
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
    const StringData scoreFusionScoreDetailsDescription,
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
                                ScoreFusionPipelineBuilder::kScoreFusionInternalFieldsName,
                                "calculatedScoreDetails")),
            expCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}

/**
 * Append logic for applying scoreNulls behavior for the "rawScore" field.
 */
void ScoreFusionPipelineBuilder::groupDocsByIdAcrossInputPipelineScoreDetails(
    StringData pipelineName, BSONObjBuilder& pushBob) {
    const std::string rawScoreName = fmt::format("{}_rawScore", pipelineName);
    pushBob.append(rawScoreName,
                   BSON("$ifNull" << BSON_ARRAY(
                            fmt::format("${}",
                                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                            getInternalFieldsName(), rawScoreName))
                            << 0)));
}

/**
 * Append logic for determining the max value of the "rawScore" field across documents.
 */
void ScoreFusionPipelineBuilder::projectReduceInternalFieldsScoreDetails(
    BSONObjBuilder& bob, StringData pipelineName, const bool forInitialValue) {
    if (forInitialValue) {
        bob.append(fmt::format("{}_rawScore", pipelineName), 0);
    } else {
        bob.append(fmt::format("{}_rawScore", pipelineName),
                   BSON("$max" << BSON_ARRAY(fmt::format("$$value.{}_rawScore", pipelineName)
                                             << fmt::format("$$this.{}_rawScore", pipelineName))));
    }
}

/**
 * Append logic for adding the $scoreFusion-specific input pipeline scoreDetails values (rawScore,
 * weight, and value).
 */
void ScoreFusionPipelineBuilder::constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
    BSONObjBuilder& bob, StringData pipelineName, double weight) {
    bob.append("inputPipelineRawScore"_sd, fmt::format("${}_rawScore", pipelineName));
    bob.append("weight"_sd, weight);
    bob.append("value"_sd, fmt::format("${}_score", pipelineName));
}

/**
 * Build stages for input pipeline. Example where the input pipeline is called "name1" and has a
 * weight of 5.0:
 * { ... stages of input pipeline pipeline ... }
 * { "$replaceRoot": { "newRoot": { "<INTERNAL_DOCS>.": "$$ROOT" } } },
 * { "$addFields": { "<INTERNAL_FIELDS>.name1_score": { "$multiply": [ { $meta: "score" }, {
 * "$const": 5.0 } ] } } }
 * If scoreDetails is true, include the following stages:
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_rawScore: { "$meta": "score" } } }
 * {$addFields: {<INTERNAL_FIELDS>.<inputPipelineName>_scoreDetails: ...} }
 * If normalization is minMaxScaler, inlude the following stage:
 * {$setWindowFields: {sortBy: ...}}
 */
std::list<boost::intrusive_ptr<DocumentSource>>
ScoreFusionPipelineBuilder::buildInputPipelineDesugaringStages(
    StringData inputPipelineOneName,
    double weight,
    const std::unique_ptr<Pipeline>& pipeline,
    bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    ScoreFusionNormalizationEnum normalization = getSpec().getInput().getNormalization();
    for (auto&& stage : pipeline->getSources()) {
        outputStages.emplace_back(stage);
    }

    outputStages.emplace_back(buildReplaceRootStage(expCtx));
    outputStages.emplace_back(
        buildScoreAddFieldsStage(expCtx, inputPipelineOneName, normalization, weight));

    // TODO SERVER-105867: Investigate why these two stages have to happen on the shard and not on
    // the merging node in order for $score's scoreDetails to be populated correctly.
    if (shouldIncludeScoreDetails()) {
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
 * Calculate the final score field to add to each document, sorts the documents by score and id, and
 * removes all internal processing fields.

 * The $sort stage looks like this: { "$sort": { "score": {$meta: "score"}, "_id": 1 } }
 *
 * When scoreDetails is enabled, the $score metadata will be set after the grouping behavior
 * described above, then the final scoreDetails object will be calculated, the $scoreDetails
 * metadata will be set, and then the $sort and final exclusion $project stages will follow.
 */
std::list<boost::intrusive_ptr<DocumentSource>>
ScoreFusionPipelineBuilder::buildScoreAndMergeStages(
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // The ScoreFusionScoringOptions class sets the combination.method and combination.expression to
    // the correct user input after performing the necessary error checks (ex: verify that if
    // combination.method is 'custom', then the combination.expression should've been specified).
    // Average is the default combination method if no other method is specified.
    ScoreFusionScoringOptions metadata(getSpec());
    std::list<boost::intrusive_ptr<DocumentSource>> scoreAndMergeStages;

    // Set the final score.
    scoreAndMergeStages.emplace_back(
        buildSetFinalCombinedScoreStage(expCtx, pipelineNames, metadata));

    // Note that the scoreDetails fields go here in the pipeline. We create them below to be
    // able to return them immediately once all stages are generated.
    const SortPattern sortingPattern{BSON("score" << BSON("$meta" << "score") << "_id" << 1),
                                     expCtx};
    auto sort = DocumentSourceSort::create(expCtx, sortingPattern);

    // Calculate score details, if necessary.
    if (shouldIncludeScoreDetails()) {
        auto addFieldsScoreDetails =
            constructCalculatedFinalScoreDetails(pipelineNames, weights, expCtx);
        auto setScoreDetails =
            constructScoreDetailsMetadata(metadata, getScoreDetailsDescription(), expCtx);
        scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                                   {std::move(addFieldsScoreDetails), std::move(setScoreDetails)});
    }

    // Remove the internal fields object.
    auto removeInternalFieldsProject = DocumentSourceProject::createFromBson(
        projectRemoveInternalFieldsObject().firstElement(), expCtx);
    scoreAndMergeStages.splice(scoreAndMergeStages.end(),
                               {std::move(sort), std::move(removeInternalFieldsProject)});
    return scoreAndMergeStages;
}
}  // namespace mongo
