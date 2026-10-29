// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * The $rankFusion stage is syntactic sugar for generating an output of ranked results by combining
 * the results of any number of ranked subpipelines with reciprocal rank fusion.
 *
 * You can see an sample desugared pipeline in ranked_fusion_verbose_replace_root_test.js, but
 * conceptually, this stage, given n input pipelines each with a unique name, desugars into a
 * pipeline consisting of:
 * - The first input pipeline (e.g. $vectorSearch).
 * - $group, $unwind and $addFields that for each document returned will:
 *     - Add a rank field: <pipeline name>_rank (e.g. vs_rank).
 *     - Add a score field: <pipeline name>_score (e.g. vs_score).
 *         - Score is calculated with the formula 1 / (rank + rankConstant). Currently rankConstant
 *           is set to 60.
 * - n-1 $unionWith stages on the same collection, which take as input pipelines:
 *     - The nth input pipeline.
 *     - $group, $unwind and $addFields which do the same thing as described above.
 * - $group by ID and turn null scores into 0.
 * - $addFields for a 'score' field which will add the n scores for each document.
 * - $sort in descending order.
 */
class DocumentSourceRankFusion final {
public:
    static constexpr std::string_view kStageName = "$rankFusion"sv;

    /**
     * Returns a list of stages to execute hybrid scoring with rank fusion.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Creates DocumentSource from StageParams. Used when featureFlagExtensionsInsideHybridSearch
     * is enabled to support extension stages in rank fusion input pipelines.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromStageParams(
        const RankFusionStageParams& params,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceRankFusion directly, use createFromBson() instead.
    DocumentSourceRankFusion() = delete;
};
}  // namespace mongo
