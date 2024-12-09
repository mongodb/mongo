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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/allowed_contexts.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(scoreFusion,
                                           DocumentSourceScoreFusion::LiteParsed::parse,
                                           DocumentSourceScoreFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagSearchHybridScoring);

namespace {

/**
 * Checks is this stage is a $score stage, where it has been desugared to $setMetadata with the meta
 * type MetaType::kScore.
 */
bool isScoreStage(boost::intrusive_ptr<DocumentSource> stage) {
    if (stage->getSourceName() != DocumentSourceSetMetadata::kStageName) {
        return false;
    }
    auto singleDocTransform = static_cast<DocumentSourceSingleDocumentTransformation*>(stage.get());
    auto setMetadataTransform =
        static_cast<SetMetadataTransformation*>(&singleDocTransform->getTransformer());
    return setMetadataTransform->getMetaType() == DocumentMetadataFields::MetaType::kScore;
}

/**
 * Checks that the input pipeline is a valid scored pipeline. This means it is either one of
 * $search, $vectorSearch, $scoreFusion, $rankFusion (which have scored output) or has an explicit
 * $score stage. A scored pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void scoreFusionPipelineValidator(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyScoredStages{DocumentSourceVectorSearch::kStageName,
                                                             DocumentSourceSearch::kStageName};
    auto sources = pipeline.getSources();
    auto firstStageName = sources.front()->getSourceName();
    auto isScoredPipeline = implicitlyScoredStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), isScoreStage);
    uassert(
        9402500,
        str::stream()
            << "All subpipelines to the $scoreFusion stage must begin with one of $search, "
               "$vectorSearch, $rankFusion, $scoreFusion or have a custom $score in the pipeline.",
        isScoredPipeline);


    std::for_each(sources.begin(), sources.end(), [](auto& stage) {
        if (stage->getSourceName() == DocumentSourceSearch::kStageName) {
            uassert(
                9402501,
                str::stream()
                    << "$search can be used in a $scoreFusion subpipeline but not when "
                       "returnStoredSource is set to true because it modifies the output fields. "
                       "Only stages that retrieve, limit, or order documents are allowed.",
                stage->constraints().noFieldModifications);
        } else {
            uassert(9402502,
                    str::stream() << stage->getSourceName()
                                  << " is not allowed in a $scoreFusion subpipeline because it "
                                     "modifies the documents or transforms their fields. Only "
                                     "stages that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        }
    });
}
}  // namespace

std::unique_ptr<DocumentSourceScoreFusion::LiteParsed> DocumentSourceScoreFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::Object);

    std::vector<LiteParsedPipeline> liteParsedPipelines;

    auto parsedSpec = ScoreFusionSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());

    // Ensure that all pipelines are valid scored selection pipelines.
    for (const auto& input : parsedSpec.getInputs()) {
        liteParsedPipelines.emplace_back(LiteParsedPipeline(nss, input.getPipeline()));
    }

    return std::make_unique<DocumentSourceScoreFusion::LiteParsed>(spec.fieldName(),
                                                                   std::move(liteParsedPipelines));
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);
    auto spec = ScoreFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    std::list<std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    // Ensure that all pipelines are valid scored selection pipelines.
    for (const auto& input : spec.getInputs()) {
        inputPipelines.push_back(
            Pipeline::parse(input.getPipeline(),
                            pExpCtx->copyForSubPipeline(pExpCtx->getNamespaceString()),
                            scoreFusionPipelineValidator));
    }

    // TODO SERVER-94022: Desugar $scoreFusion (will return something then)
    return {};
}
}  // namespace mongo
