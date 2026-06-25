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
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/score_fusion_pipeline_builder.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(
    scoreFusion,
    LiteParsedScoreFusion::parse,
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagSearchHybridScoringFull);

namespace {
DocumentSourceContainer scoreFusionStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<ScoreFusionStageParams*>(stageParams.get());
    tassert(12192020,
            "Expected ScoreFusionStageParams in $scoreFusion dispatch",
            typedParams != nullptr);
    // TODO SERVER-121094 Remove the createFromBson path when the feature flag is deleted.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (hybridSearchFlagEnabled) {
        return DocumentSourceScoreFusion::createFromStageParams(*typedParams, expCtx);
    }
    return DocumentSourceScoreFusion::createFromBson(typedParams->getOriginalBson(), expCtx);
}
}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(scoreFusion,
                                                 ScoreFusionStageParams::id,
                                                 scoreFusionStageParamsToDocumentSourceFn);

/**
 * Checks that the input pipeline is a valid scored pipeline. This means it is either one of
 * $search, $vectorSearch, $scoreFusion, $rankFusion (which have scored output) or has an explicit
 * $score stage. A scored pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 *
 * Input pipeline validation happens during the explicit validation phase at lite-parse time
 * (LiteParsedScoreFusion::validate()) before parsing into a `Pipeline` object.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseScoredSelectionPipelines(
    const ScoreFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // TODO SERVER-117661: Implement support for extension $vectorSearch stages in
    // scoreFusion pipelines.

    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    for (const auto& innerPipelineBsonElem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(innerPipelineBsonElem);
        LiteParsedPipeline liteParsedPipeline(
            pExpCtx->getNamespaceString(),
            bsonPipeline,
            false,
            LiteParserOptions{.ifrContext = pExpCtx->getIfrContext()});
        auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, pExpCtx);

        // Post-condition sanity check: LiteParsedScoreFusion::validate() already asserted the
        // input pipeline produces score metadata; confirm the fully-parsed pipeline agrees.
        tassert(
            10535800,
            "The metadata dependency tracker determined $scoreFusion input pipeline does not "
            "generate score metadata, despite the input pipeline stages being previously validated "
            "as such.",
            pipeline->generatesMetadataType(DocumentMetadataFields::kScore));

        inputPipelines[innerPipelineBsonElem.fieldName()] = std::move(pipeline);
    }
    return inputPipelines;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromStageParams(
    const ScoreFusionStageParams& params, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const auto& spec = params.getSpec();
    const auto& inputPipelines = params.buildInputPipelines(pExpCtx);

    StringMap<double> weights;
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value() && combinationSpec->getWeights().has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights()->getOwned(), inputPipelines, kStageName);
    }

    ScoreFusionPipelineBuilder builder(spec, weights);
    return builder.constructDesugaredOutput(inputPipelines, pExpCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = ScoreFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseScoredSelectionPipelines(spec, pExpCtx);

    StringMap<double> weights;
    // If ScoreFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value() && combinationSpec->getWeights().has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights()->getOwned(), inputPipelines, kStageName);
    }

    ScoreFusionPipelineBuilder builder(spec, weights);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages =
        builder.constructDesugaredOutput(inputPipelines, pExpCtx);

    return outputStages;
}
}  // namespace mongo
