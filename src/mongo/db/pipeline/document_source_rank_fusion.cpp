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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/ifr_flag_retry_info.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/rank_fusion_pipeline_builder.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/util/rank_fusion_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <algorithm>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(rankFusion,
                                     LiteParsedRankFusion::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

namespace {
DocumentSourceContainer rankFusionStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<RankFusionStageParams*>(stageParams.get());
    tassert(
        12192021, "Expected RankFusionStageParams in $rankFusion dispatch", typedParams != nullptr);
    // TODO SERVER-121094 Remove the createFromBson path when the feature flag is deleted.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (hybridSearchFlagEnabled) {
        return DocumentSourceRankFusion::createFromStageParams(*typedParams, expCtx);
    }
    return DocumentSourceRankFusion::createFromBson(typedParams->getOriginalBson(), expCtx);
}
}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(rankFusion,
                                                 RankFusionStageParams::id,
                                                 rankFusionStageParamsToDocumentSourceFn);

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

/**
 * Validate that each pipeline is a valid ranked selection pipeline. Returns a pair of the map of
 * the input pipeline names to pipeline objects and a map of pipeline names to score paths.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseAndValidateRankedSelectionPipelines(
    const RankFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    for (const auto& innerPipelineBsonElem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(innerPipelineBsonElem);
        // Ensure that all pipelines are valid ranked selection pipelines.
        rankFusionBsonPipelineValidator(bsonPipeline);

        LiteParsedPipeline liteParsedPipeline =
            LiteParsedPipeline(pExpCtx->getNamespaceString(),
                               bsonPipeline,
                               false,
                               LiteParserOptions{.ifrContext = pExpCtx->getIfrContext()});

        // If featureFlagExtensionsInsideHybridSearch is not enabled, perform IFR kickback
        // for any input pipeline that has an extension $vectorSearch or $search stage.
        auto ifrCtx = pExpCtx->getIfrContext();
        auto hybridSearchFlagEnabled = ifrCtx &&
            ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
        if (!hybridSearchFlagEnabled) {
            // TODO SERVER-115791: Implement support for extension $vectorSearch stages in
            // rankFusion pipelines.
            search_helpers::throwIfrKickbackIfNecessary(
                liteParsedPipeline.hasExtensionVectorSearchStage(),
                feature_flags::gFeatureFlagVectorSearchExtension,
                vector_search_metrics::inHybridSearchKickbackRetryCount,
                "$vectorSearch-as-an-extension is not allowed in a $rankFusion pipeline.");

            search_helpers::throwIfrKickbackIfNecessary(
                liteParsedPipeline.hasExtensionSearchStage(),
                feature_flags::gFeatureFlagSearchExtension,
                search_metrics::inHybridSearchKickbackRetryCount,
                "$search-as-an-extension is not allowed in a $rankFusion pipeline.");
        }

        auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, pExpCtx);

        // Validate pipeline name.
        auto inputName = innerPipelineBsonElem.fieldName();
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

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromStageParams(
    const RankFusionStageParams& params, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "'featureFlagRankFusionBasic' must be enabled to use rankFusion",
            bypassRankFusionFCVGate || pExpCtx->isBasicRankFusionFeatureFlagEnabled());

    const auto& spec = params.getSpec();
    const auto& inputPipelines = params.buildInputPipelines(pExpCtx);

    StringMap<double> weights;
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, kStageName);
    }

    RankFusionPipelineBuilder builder(spec, weights);
    return builder.constructDesugaredOutput(inputPipelines, pExpCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "'featureFlagRankFusionBasic' must be enabled to use rankFusion",
            bypassRankFusionFCVGate || pExpCtx->isBasicRankFusionFeatureFlagEnabled());

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = RankFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseAndValidateRankedSelectionPipelines(spec, pExpCtx);

    StringMap<double> weights;
    // If RankFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, kStageName);
    }

    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (spec.getScoreDetails()) {
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                "'featureFlagRankFusionFull' must be enabled to use scoreDetails",
                isRankFusionFullEnabled());
    }

    RankFusionPipelineBuilder builder(spec, weights);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages =
        builder.constructDesugaredOutput(inputPipelines, pExpCtx);

    return outputStages;
}
}  // namespace mongo
