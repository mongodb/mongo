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

#include "mongo/db/pipeline/document_source_score.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_score_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

/**
 * Register $score as a DocumentSource without feature flag and check that the hybrid scoring
 * feature flag is enabled in createFromBson() instead of via
 * REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG. This method of feature flag checking avoids hitting
 * QueryFeatureNotAllowed and duplicate parser map errors in $scoreFusion tests ($scoreFusion is
 * gated behind the same feature flag).
 */
REGISTER_DOCUMENT_SOURCE(score,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceScore::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

namespace {
static const std::string scoreScoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized or "
    "nonnormalized value:";

// Internal, intermediate top-level field name used for the raw score calculated that will be put
// into scoreDetails. This is required because the 'score' expression in $score can only be
// calculated once, in case it has a recursive reference to {$meta: score}.
static constexpr StringData kInternalRawScoreField = "internal_raw_score";

// Internal, intermediate top-level field name used for minMaxScaler normalization. The
// $minMaxScaler is output into this field during intermediate processing, and then written back to
// the score metadata variable.
static constexpr StringData kInternalMinMaxScalerNormalizationField =
    "internal_min_max_scaler_normalization_score"_sd;

/**
 * Builds a $setMetadata expression to set the score metadata variable.
 */
boost::intrusive_ptr<DocumentSource> buildSetMetadataStageFromExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression>&& expr) {
    return DocumentSourceSetMetadata::create(
        expCtx, std::move(expr), DocumentMetadataFields::MetaType::kScore);
}

/**
 * Builds and returns a $replaceRoot stage: {$replaceWith: {docs: "$$ROOT"}}.
 * This has the effect of storing the unmodified user's document in the path '$docs'.
 */
boost::intrusive_ptr<DocumentSource> buildReplaceRootStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON("docs" << "$$ROOT")).firstElement(), expCtx);
}

/**
 * Builds and returns a $replaceRoot stage: {$replaceRoot: {"newRoot": "$docs"}}.  This restores the
 * user's document.
 */
boost::intrusive_ptr<DocumentSource> buildRestoreRootStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceReplaceRoot::create(expCtx,
                                             ExpressionFieldPath::createPathFromString(
                                                 expCtx.get(), "docs", expCtx->variablesParseState),
                                             "documents",
                                             SbeCompatibility::noRequirements);
}

/**
 * Builds the set of stages required to calculate and evaluate the raw score expression, then sets
 * the "score" metadata variable. Note that the calculated score is both unnormalized and
 * unweighted. Currently, this helper function builds the following subpipeline:
 * [
 *     {
 *         $setMetadata: {
 *             score: <evaluate_score_expression>
 *         }
 *     }
 * ]
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildRawScoreCalculationStages(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    auto scoreExpression = Expression::parseOperand(
        expCtx.get(), spec.getScore().getElement(), expCtx->variablesParseState);
    outputStages.emplace_back(
        buildSetMetadataStageFromExpression(expCtx, std::move(scoreExpression)));

    return outputStages;
}

/**
 * Builds the set of stages to prep for scoreDetails.  This includes a $replaceRoot stage in order
 * to hide the customer's doc so that we can do our own processing, and an $addFields to preserve
 * the raw score such that we do not need to calculate it again later for scoreDetails. Calculating
 * the raw score twice may not even be possible, in the case where the raw score depends on the
 * value of {$meta: score}, which is updated throughout the desugared pipeline of $score. Currently,
 * this helper function builds the following subpipeline:
 * [
 *     {
 *         $replaceRoot: {
 *             newRoot: {
 *                 docs: "$$ROOT"
 *             }
 *         }
 *     },
 *     {
 *         $addFields: {
 *             internal_raw_score: {
 *                 $meta: "score"
 *             }
 *         }
 *     }
 * ]
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildScoreDetailsPreparationStages(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    outputStages.emplace_back(buildReplaceRootStage(expCtx));

    auto scoreExpression = Expression::parseObject(
        expCtx.get(), BSON("$meta" << "score"), expCtx->variablesParseState);
    outputStages.emplace_back(DocumentSourceAddFields::create(
        FieldPath(kInternalRawScoreField), std::move(scoreExpression), expCtx));

    return outputStages;
}

/**
 * Builds the set of stages required to calculate the sigmoid normalization of the score. Note that
 * the resulting score is normalized, but unweighted. Currently, this helper function builds the
 * following subpipeline:
 * [
 *     {
 *         $setMetadata: {
 *             score: <sigmoid_expression>
 *         }
 *     }
 * ]
 */
std::list<boost::intrusive_ptr<DocumentSource>>
buildNormalizationCalculationStagesForSigmoidNormalization(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(9460009,
            "Expected normalization to be sigmoid",
            spec.getNormalization() == ScoreNormalizationEnum::kSigmoid);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    auto sigmoidExpression = ExpressionSigmoid::parseExpressionSigmoid(
        expCtx.get(),
        BSON("" << BSON("$meta" << "score")).firstElement(),
        expCtx->variablesParseState);
    outputStages.emplace_back(
        buildSetMetadataStageFromExpression(expCtx, std::move(sigmoidExpression)));

    return outputStages;
}

/**
 * Builds and returns a $setWindowFields stage to calculate the minMaxScaler normalization.
 */
boost::intrusive_ptr<DocumentSource> buildSetWindowFieldsStage(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(9460005,
            "Expected normalization to be minMaxScaler",
            spec.getNormalization() == ScoreNormalizationEnum::kMinMaxScaler);

    const std::string score = std::string{kInternalMinMaxScalerNormalizationField};
    SortPattern sortPattern{BSON(score << -1), expCtx};

    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        boost::none,
        sortPattern,
        std::vector<WindowFunctionStatement>{WindowFunctionStatement{
            score,  // output field
            window_function::Expression::parse(
                BSON("$minMaxScaler" << BSON("input" << BSON("$meta" << "score"))),
                sortPattern,
                expCtx.get())}},
        SbeCompatibility::notCompatible);
}

/**
 * Builds and returns a $setMetadata stage to set the score metadata variable to the value of the
 * output of the $setWindowFields stage. This is necessary because it is not possible to have
 * $setWindowFields output to a metadata variable.
 */
boost::intrusive_ptr<DocumentSource> buildSetMetadataStageForMinMaxScalerOutput(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(9460007,
            "Expected normalization to be minMaxScaler",
            spec.getNormalization() == ScoreNormalizationEnum::kMinMaxScaler);

    const std::string dollarScore = "$" + kInternalMinMaxScalerNormalizationField;
    auto scoreExpression = Expression::parseOperand(
        expCtx.get(), BSON("" << dollarScore).firstElement(), expCtx->variablesParseState);
    return buildSetMetadataStageFromExpression(expCtx, std::move(scoreExpression));
}

/**
 * Builds the set of stages required to calculate the minMaxScaler normalization of the score. Note
 * that the resulting score is normalized, but unweighted. This helper function runs the score
 * metadata variable through the $minMaxScaler window function. To see the exact desugared output,
 * look at document_source_score_test.cpp.
 */
std::list<boost::intrusive_ptr<DocumentSource>>
buildNormalizationCalculationStagesForMinMaxScalerNormalization(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(9460008,
            "Expected normalization to be minMaxScaler",
            spec.getNormalization() == ScoreNormalizationEnum::kMinMaxScaler);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    outputStages.emplace_back(buildSetWindowFieldsStage(spec, expCtx));
    outputStages.emplace_back(buildSetMetadataStageForMinMaxScalerOutput(spec, expCtx));

    return outputStages;
}

/**
 * Builds the set of stages required to calculate the normalized score.
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildNormalizationCalculationStages(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    switch (spec.getNormalization()) {
        case ScoreNormalizationEnum::kNone: {
            return {};
        }
        case ScoreNormalizationEnum::kSigmoid: {
            return buildNormalizationCalculationStagesForSigmoidNormalization(spec, expCtx);
        }
        case ScoreNormalizationEnum::kMinMaxScaler: {
            return buildNormalizationCalculationStagesForMinMaxScalerNormalization(spec, expCtx);
        }
    }
    MONGO_UNREACHABLE_TASSERT(9460010);
}

/**
 * Builds the set of stages required to calculate the weighted score from the unweighted and
 * normalized score. Currently, this helper function builds the following subpipeline (if a weight
 * is provided):
 * [
 *     {
 *         $setMetadata: {
 *             score: {
 *                 $multiply: {
 *                     [{$meta: "score", {"$const": <weight>}}]
 *                 }
 *             }
 *         }
 *     }
 * ]
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildWeightCalculationStages(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (spec.getWeight() == 1) {
        // No calculation is necessary, the weighted score is the same as the unweighted score.
        return {};
    }
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    auto scoreMetadataExpression =
        Expression::parseOperand(expCtx.get(),
                                 BSON("" << BSON("$meta" << "score")).firstElement(),
                                 expCtx->variablesParseState);
    std::vector<intrusive_ptr<Expression>> multiplyChildren = {
        std::move(scoreMetadataExpression),
        make_intrusive<ExpressionConstant>(expCtx.get(), Value(spec.getWeight()))};
    auto multiplyExpression =
        make_intrusive<ExpressionMultiply>(expCtx.get(), std::move(multiplyChildren));
    outputStages.emplace_back(
        buildSetMetadataStageFromExpression(expCtx, std::move(multiplyExpression)));

    return outputStages;
}

std::string getNormalizationString(ScoreNormalizationEnum normalization) {
    switch (normalization) {
        case ScoreNormalizationEnum::kSigmoid:
            return "sigmoid";
        case ScoreNormalizationEnum::kMinMaxScaler:
            return "minMaxScaler";
        case ScoreNormalizationEnum::kNone:
            return "none";
        default:
            // Only one of the above options can be specified for normalization.
            MONGO_UNREACHABLE_TASSERT(9467000);
    }
}

/**
 * Builds the scoreDetails metadata for $score.
 *     {
 *         $setMetadata: {
 *             scoreDetails: {
 *                 value: { $meta: "score" },
 *                 description: { $const: "the score calculated from..." },
 *                 rawScore: "$myScore", // user input to $score.score
 *                 normalization: { $const: "none" }, // user input to $score.normalization
 *                 weight: { $const: 1 }, // user input to $score.weight
 *                 expression: "{ string: { $add: [ '$myScore', '$otherScore' ] } }",
 *                 details: []
 *             }
 *         }
 *     }
 */
boost::intrusive_ptr<DocumentSource> setScoreDetailsMetadata(
    ScoreSpec spec, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    // To calculate the 'rawScore' field, we recompute the input score expression from scratch, even
    // though we'd already computed it prior to normalization. It's possible to save this
    // intermediate value to avoid recomputation. However, doing so would generate a more complex
    // desugared output and implementation. Thus, we decided this approach is better overall.
    boost::intrusive_ptr<DocumentSource> setScoreDetails = DocumentSourceSetMetadata::create(
        pExpCtx,
        Expression::parseObject(
            pExpCtx.get(),
            BSON("value" << BSON("$meta" << "score") << "description"
                         << scoreScoreDetailsDescription << "rawScore"
                         << ("$" + kInternalRawScoreField) << "normalization"
                         << getNormalizationString(spec.getNormalization()) << "weight"
                         << spec.getWeight() << "expression"
                         << hybrid_scoring_util::score_details::stringifyExpression(spec.getScore())
                         << "details" << BSONArrayBuilder().arr()),
            pExpCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}

}  // namespace

std::list<boost::intrusive_ptr<DocumentSource>> constructDesugaredOutput(
    const ScoreSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    // std::list.splice() has a runtime complexity of O(1), because under the hood it just reassigns
    // internal pointers.
    outputStages.splice(outputStages.end(), buildRawScoreCalculationStages(spec, pExpCtx));

    // Note that the scoreDetails preparation stages ($replaceRoot and $addFields) must happen
    // *after* raw score calculation for two reasons. The first is that the raw score can't be set
    // until it is calculated. The second is that the raw score calculation may depend on fields
    // inside the user's document, and running the $replaceRoot before calculation would remove
    // access to those fields (without modification to references to add the 'docs.' prefix).
    outputStages.splice(outputStages.end(), buildScoreDetailsPreparationStages(pExpCtx));

    outputStages.splice(outputStages.end(), buildNormalizationCalculationStages(spec, pExpCtx));
    outputStages.splice(outputStages.end(), buildWeightCalculationStages(spec, pExpCtx));

    const bool includeScoreDetails = spec.getScoreDetails();
    if (includeScoreDetails) {
        outputStages.emplace_back(setScoreDetailsMetadata(spec, pExpCtx));
    }
    outputStages.emplace_back(buildRestoreRootStage(pExpCtx));

    return outputStages;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScore::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(
        ErrorCodes::QueryFeatureNotAllowed,
        "$score is not allowed in the current configuration. You may need to enable the "
        "corresponding feature flag",
        feature_flags::gFeatureFlagSearchHybridScoringFull.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(pExpCtx->getOperationContext()),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = ScoreSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    return constructDesugaredOutput(spec, pExpCtx);
}

}  // namespace mongo
