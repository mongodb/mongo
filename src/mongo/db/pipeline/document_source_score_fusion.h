// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * The $scoreFusion stage is syntactic sugar for generating an output of scored results by combining
 * the results of any number of scored subpipelines with relative score fusion.
 *
 * Given n input pipelines each with a unique name, desugars into a
 * pipeline consisting of:
 * - The first input pipeline (e.g. $vectorSearch).
 * - $replaceRoot and $addFields that for each document returned will:
 *     - Add a score field: <pipeline name>_score (e.g. vs_score).
 *         - Score is calculated as the weight * the score field on the input documents.
 * - n-1 $unionWith stages on the same collection, which take as input pipelines:
 *     - The nth input pipeline.
 *     - $replaceRoot and $addFields which do the same thing as described above.
 * - $group by ID and turn null scores into 0.
 * - $addFields for a 'score' field which will aggregate the n scores for each document.
 * - $sort in descending order.
 */
class DocumentSourceScoreFusion final {
public:
    static constexpr std::string_view kStageName = "$scoreFusion"sv;

    /**
     * Returns a list of stages to execute hybrid scoring with score fusion.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Creates DocumentSource from StageParams. Used when featureFlagExtensionsInsideHybridSearch
     * is enabled to support extension stages in score fusion input pipelines.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromStageParams(
        const ScoreFusionStageParams& params,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceScoreFusion directly, use createFromBson()
    // instead.
    DocumentSourceScoreFusion() = delete;
};
}  // namespace mongo
