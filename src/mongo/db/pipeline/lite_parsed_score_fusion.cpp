// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_score_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/ifr_flag_retry_info.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(scoreFusion, ScoreFusionStageParams::id);

std::unique_ptr<LiteParsedScoreFusion> LiteParsedScoreFusion::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << DocumentSourceScoreFusion::kStageName
                          << " must take a nested object but found: " << spec,
            spec.type() == BSONType::object);

    const bool extensionsInHybridSearchEnabled = options.ifrContext &&
        options.ifrContext->getSavedFlagValue(
            feature_flags::gFeatureFlagExtensionsInsideHybridSearch);

    auto parsedSpec = ScoreFusionSpec::parse(
        spec.embeddedObject(), IDLParserContext(DocumentSourceScoreFusion::kStageName));

    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Only parse input pipelines here. All semantic validation happens in validate().
    std::vector<OwnedLiteParsedPipeline> ownedPipelines;
    for (const auto& elem : inputPipesObj) {
        auto bsonPipeline = parsePipelineFromBSON(elem);
        ownedPipelines.emplace_back(nss, bsonPipeline, options);
    }

    return std::make_unique<LiteParsedScoreFusion>(spec,
                                                   nss,
                                                   std::move(parsedSpec),
                                                   std::move(ownedPipelines),
                                                   extensionsInHybridSearchEnabled);
}

void LiteParsedScoreFusion::validate() const {
    static const std::string scorePipelineMsg =
        "All input pipelines to the $scoreFusion stage must begin with one of $search, "
        "$vectorSearch, or have a custom $score in the pipeline.";

    // Validate pipeline names are unique. Uses the parsed spec which owns the data.
    StringDataSet seenNames;
    for (const auto& elem : _parsedSpec.getInput().getPipelines()) {
        auto pipelineName = elem.fieldNameStringData();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(pipelineName),
            "$scoreFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(12108715,
                str::stream()
                    << "$scoreFusion pipeline names must be unique, but found duplicate name '"
                    << pipelineName << "'.",
                seenNames.insert(pipelineName).second);
    }

    for (const auto& ownedPipeline : _pipelines) {
        const auto& pipeline = *ownedPipeline;
        const auto& stages = pipeline.getStages();

        // Each input pipeline must not be empty.
        uassert(12108710,
                str::stream() << "$scoreFusion input pipeline cannot be empty. "
                              << scorePipelineMsg,
                !stages.empty());

        // IFR kickback must fire BEFORE scored/selection checks. When the extensions-inside-
        // hybrid-search flag is ON we are already in the correct code path; suppress the kickback.
        if (!_extensionsInHybridSearchEnabled) {
            search_helpers::throwIfrKickbackIfNecessary(
                pipeline.hasExtensionVectorSearchStage(),
                feature_flags::gFeatureFlagVectorSearchExtension,
                vector_search_metrics::inHybridSearchKickbackRetryCount,
                "$vectorSearch-as-an-extension is not allowed in a $scoreFusion pipeline.");

            search_helpers::throwIfrKickbackIfNecessary(
                pipeline.hasExtensionSearchStage(),
                feature_flags::gFeatureFlagSearchExtension,
                search_metrics::inHybridSearchKickbackRetryCount,
                "$search-as-an-extension is not allowed in a $scoreFusion pipeline.");
        }

        // No nested hybrid search stages ($rankFusion/$scoreFusion).
        uassert(12108711,
                "$scoreFusion input pipeline has a nested hybrid search stage "
                "($rankFusion/$scoreFusion). " +
                    scorePipelineMsg,
                !pipeline.hasHybridSearchStage());

        // LiteParsedExpandable delegates isScoredStage() to its expanded stages, so extension
        // stages report scored-ness correctly.
        uassert(12108712,
                "Pipeline did not begin with a scored stage and did not contain an explicit "
                "$score stage. " +
                    scorePipelineMsg,
                pipeline.isScoredPipeline());

        // All stages must be selection stages.
        pipeline.validateAllStagesAreSelection(12108713, "$scoreFusion");
    }
}

std::map<std::string, std::unique_ptr<Pipeline>> ScoreFusionStageParams::buildInputPipelines(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx) const {
    tassert(12192010,
            "$scoreFusion: mismatch between spec and parsed input pipelines",
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
