// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/util/rank_fusion_util.h"

#include <string_view>

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(rankFusion, RankFusionStageParams::id);

std::unique_ptr<LiteParsedRankFusion> LiteParsedRankFusion::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << DocumentSourceRankFusion::kStageName
                          << " must take a nested object but found: " << spec,
            spec.type() == BSONType::object);

    const bool extensionsInHybridSearchEnabled = options.ifrContext &&
        options.ifrContext->getSavedFlagValue(
            feature_flags::gFeatureFlagExtensionsInsideHybridSearch);

    auto parsedSpec = RankFusionSpec::parse(spec.embeddedObject(),
                                            IDLParserContext(DocumentSourceRankFusion::kStageName));

    if (parsedSpec.getScoreDetails()) {
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                "'featureFlagRankFusionFull' must be enabled to use scoreDetails",
                isRankFusionFullEnabled());
    }

    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Only parse input pipelines here. All semantic validation happens in validate().
    std::vector<OwnedLiteParsedPipeline> ownedPipelines;
    for (const auto& elem : inputPipesObj) {
        auto bsonPipeline = parsePipelineFromBSON(elem);
        ownedPipelines.emplace_back(nss, bsonPipeline, options);
    }

    return std::make_unique<LiteParsedRankFusion>(spec,
                                                  nss,
                                                  std::move(parsedSpec),
                                                  std::move(ownedPipelines),
                                                  extensionsInHybridSearchEnabled);
}

void LiteParsedRankFusion::validate(const OperationContext* opCtx) const {
    static const std::string rankPipelineMsg =
        "All input pipelines to the $rankFusion stage must begin with one of $search, "
        "$vectorSearch, $geoNear, or have a $sort in the pipeline.";

    // Validate pipeline names are unique. Uses the parsed spec which owns the data,
    // so std::string_view from fieldNameStringData() is safe (no use-after-free).
    StringDataSet seenNames;
    for (const auto& elem : _parsedSpec.getInput().getPipelines()) {
        auto pipelineName = elem.fieldNameStringData();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(pipelineName),
            "$rankFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(
            12108714,
            str::stream() << "$rankFusion pipeline names must be unique, but found duplicate name '"
                          << pipelineName << "'.",
            seenNames.insert(pipelineName).second);
    }

    for (const auto& ownedPipeline : _pipelines) {
        const auto& pipeline = *ownedPipeline;
        const auto& stages = pipeline.getStages();

        // Each input pipeline must not be empty.
        uassert(12108700,
                str::stream() << "$rankFusion input pipeline cannot be empty. " << rankPipelineMsg,
                !stages.empty());

        // IFR kickback must fire BEFORE ranked/selection checks. When the extensions-inside-
        // hybrid-search flag is ON we are already in the correct code path; suppress the kickback.
        if (!_extensionsInHybridSearchEnabled) {
            search_helpers::throwIfrKickbackIfNecessary(
                pipeline.hasExtensionVectorSearchStage(),
                feature_flags::gFeatureFlagVectorSearchExtension,
                vector_search_metrics::inHybridSearchKickbackRetryCount,
                "$vectorSearch-as-an-extension is not allowed in a $rankFusion pipeline.");

            search_helpers::throwIfrKickbackIfNecessary(
                pipeline.hasExtensionSearchStage(),
                feature_flags::gFeatureFlagSearchExtension,
                search_metrics::inHybridSearchKickbackRetryCount,
                "$search-as-an-extension is not allowed in a $rankFusion pipeline.");
        }

        // No nested hybrid search stages ($rankFusion/$scoreFusion).
        uassert(12108701,
                "$rankFusion input pipeline has a nested hybrid search stage "
                "($rankFusion/$scoreFusion). " +
                    rankPipelineMsg,
                !pipeline.hasHybridSearchStage());

        // LiteParsedExpandable delegates isRankedStage() to its expanded stages, so extension
        // stages report ranked-ness correctly.
        uassert(12108702,
                "Pipeline did not begin with a ranked stage and did not contain an explicit "
                "$sort stage. " +
                    rankPipelineMsg,
                pipeline.isRankedPipeline());

        // Input pipelines must not contain a $score stage.
        for (const auto& stage : stages) {
            uassert(12108703,
                    "$rankFusion input pipelines must not contain a $score stage.",
                    stage->getParseTimeName() != "$score");
        }

        // All stages must be selection stages.
        pipeline.validateAllStagesAreSelection(12108704, "$rankFusion");
    }
}

std::map<std::string, std::unique_ptr<Pipeline>> RankFusionStageParams::buildInputPipelines(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx) const {
    tassert(12192000,
            "$rankFusion: mismatch between spec and parsed input pipelines",
            static_cast<size_t>(_spec.getInput().getPipelines().nFields()) == _pipelines.size());

    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    size_t i = 0;
    for (const auto& innerPipelineBsonElem : _spec.getInput().getPipelines()) {
        auto pipeline = Pipeline::parseFromLiteParsed(_pipelines[i], pExpCtx);
        inputPipelines[innerPipelineBsonElem.fieldName()] = std::move(pipeline);
        ++i;
    }
    return inputPipelines;
}

}  // namespace mongo
