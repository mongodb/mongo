/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_score_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/ifr_flag_retry_info.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/field_path.h"
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

    auto parsedSpec = ScoreFusionSpec::parse(
        spec.embeddedObject(), IDLParserContext(DocumentSourceScoreFusion::kStageName));

    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    auto opts = options;
    opts.makeSubpipelineOwned = true;

    // Only parse input pipelines here. All semantic validation happens in validate().
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    for (const auto& elem : inputPipesObj) {
        auto bsonPipeline = parsePipelineFromBSON(elem);
        liteParsedPipelines.push_back(LiteParsedPipeline(nss, bsonPipeline, false, opts));
    }

    return std::make_unique<LiteParsedScoreFusion>(
        spec, nss, std::move(parsedSpec), std::move(liteParsedPipelines));
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

    for (const auto& pipeline : _pipelines) {
        const auto& stages = pipeline.getStages();

        // Each input pipeline must not be empty.
        uassert(12108710,
                str::stream() << "$scoreFusion input pipeline cannot be empty. "
                              << scorePipelineMsg,
                !stages.empty());

        // IFR kickback must fire BEFORE scored/selection checks because extension
        // $vectorSearch doesn't report isScoredStage() on its LiteParsedExpandable.
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

        // No nested hybrid search stages ($rankFusion/$scoreFusion).
        uassert(12108711,
                "$scoreFusion input pipeline has a nested hybrid search stage "
                "($rankFusion/$scoreFusion). " +
                    scorePipelineMsg,
                !pipeline.hasHybridSearchStage());

        // Pipeline must be scored.
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
