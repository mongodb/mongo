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
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/score_fusion_pipeline_builder.h"
#include "mongo/db/query/allowed_contexts.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(
    scoreFusion,
    DocumentSourceScoreFusion::LiteParsed::parse,
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagSearchHybridScoringFull);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(scoreFusion,
                                                   DocumentSourceScoreFusion,
                                                   ScoreFusionStageParams);

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
        spec, nss, std::move(liteParsedPipelines));
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

/**
 * Validate that each pipeline is a valid scored selection pipeline. Returns a pair of the map of
 * the input pipeline names to pipeline objects and a map of pipeline names to score paths.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseAndValidateScoredSelectionPipelines(
    const ScoreFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    for (const auto& innerPipelineBsonElem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(innerPipelineBsonElem);
        // Ensure that all pipelines are valid scored selection pipelines.
        scoreFusionBsonPipelineValidator(bsonPipeline, pExpCtx);

        auto pipeline = pipeline_factory::makePipeline(
            bsonPipeline, pExpCtx, pipeline_factory::kOptionsMinimal);
        tassert(
            10535800,
            "The metadata dependency tracker determined $scoreFusion input pipeline does not "
            "generate score metadata, despite the input pipeline stages being previously validated "
            "as such.",
            (*pipeline).generatesMetadataType(DocumentMetadataFields::kScore));

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

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = ScoreFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseAndValidateScoredSelectionPipelines(spec, pExpCtx);

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
