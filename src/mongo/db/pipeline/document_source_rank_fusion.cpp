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

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/allowed_contexts.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(rankFusion,
                                           LiteParsedDocumentSourceDefault::parse,
                                           DocumentSourceRankFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagSearchHybridScoring);

namespace {
/**
 * Checks that the input pipeline is a valid ranked pipeline. This means it is either one of
 * $search, $vectorSearch, $geoNear, $rankFusion (which have ordered output) or has an explicit
 * $sort stage. A ranked pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void rankFusionPipelineValidator(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion explicitly because it will be desugared by this
    // point.
    static const std::set<StringData> implicitlyOrderedStages{
        DocumentSourceVectorSearch::kStageName,
        DocumentSourceSearch::kStageName,
        DocumentSourceGeoNear::kStageName};
    auto sources = pipeline.getSources();
    auto firstStageName = sources.front()->getSourceName();
    auto isRankedPipeline = implicitlyOrderedStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), [](auto& stage) {
                                return stage->getSourceName() == DocumentSourceSort::kStageName;
                            });
    uassert(9191100,
            str::stream()
                << "All subpipelines to the $rankFusion stage must begin with one of $search, "
                   "$vectorSearch, $geoNear, $rankFusion or have a custom $sort in the pipeline.",
            isRankedPipeline);

    std::for_each(sources.begin(), sources.end(), [](auto& stage) {
        if (stage->getSourceName() == DocumentSourceGeoNear::kStageName) {
            uassert(9191101,
                    str::stream() << "$geoNear can be used in a $rankFusion subpipeline but not "
                                     "when includeLocs or distanceField is specified because they "
                                     "modify the documents by adding an output field. Only stages "
                                     "that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        } else if (stage->getSourceName() == DocumentSourceSearch::kStageName) {
            uassert(
                9191102,
                str::stream()
                    << "$search can be used in a $rankFusion subpipeline but not when "
                       "returnStoredSource is set to true because it modifies the output fields. "
                       "Only stages that retrieve, limit, or order documents are allowed.",
                stage->constraints().noFieldModifications);
        } else {
            uassert(9191103,
                    str::stream() << stage->getSourceName()
                                  << " is not allowed in a $rankFusion subpipeline because it "
                                     "modifies the documents or transforms their fields. Only "
                                     "stages that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        }
    });
}
}  // namespace

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec = RankFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    std::list<std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& input : spec.getInputs()) {
        inputPipelines.push_back(Pipeline::parse(input.getPipeline(),
                                                 pExpCtx->copyForSubPipeline(pExpCtx->ns),
                                                 rankFusionPipelineValidator));
    }

    // TODO SERVER-92213: Implement the desugaring of $rankFusion.

    return {};
}
}  // namespace mongo
