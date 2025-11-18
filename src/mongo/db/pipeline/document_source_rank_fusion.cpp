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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/rank_fusion_pipeline_builder.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/util/rank_fusion_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#include <algorithm>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE(rankFusion,
                         DocumentSourceRankFusion::LiteParsed::parse,
                         DocumentSourceRankFusion::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

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
        spec, nss, std::move(liteParsedPipelines));
}

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

        auto pipeline = Pipeline::parse(bsonPipeline, pExpCtx);

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
