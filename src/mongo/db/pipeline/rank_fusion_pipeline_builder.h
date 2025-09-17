/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/hybrid_search_pipeline_builder.h"
#include "mongo/util/modules.h"

#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This RankFusionPipelineBuilder class stores the builder methods to build the desugared stages for
 * any given $rankFusion input pipeline and the final scoring and merging logic.
 */
class MONGO_MOD_PRIVATE RankFusionPipelineBuilder final : public HybridSearchPipelineBuilder {
    // DocumentSourceRankFusion::createFromBson() creates an instance of this class and calls its
    // inherited constructDesugaredOutput(...) method which calls the derived class' overriden
    // virtual methods to construct the final desugared output.
public:
    // Name of single top-level field object used to track all internal fields we need
    // intermediate to the desugar.
    // One field object that holds all internal intermediate variables during desugar,
    // like each input pipeline's individual score or scoreDetails.
    static constexpr StringData kRankFusionInternalFieldsName =
        "_internal_rankFusion_internal_fields"_sd;

    // One field object to encapsulate the unmodified user's doc from the queried collection.
    static constexpr StringData kRankFusionDocsFieldName = "_internal_rankFusion_docs"_sd;

    // Description that gets set as part of $rankFusion's scoreDetails metadata.
    static constexpr StringData kRankFusionScoreDetailsDescription =
        "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / "
        "(60 + rank))) across input pipelines from which this document is output, from:"_sd;

    // For now, the rankConstant is always 60.
    static constexpr double kRankConstant = 60;

private:
    RankFusionSpec _spec;
    int _rankConstant;

    std::list<boost::intrusive_ptr<DocumentSource>> buildInputPipelineDesugaringStages(
        StringData firstInputPipelineName,
        double weight,
        const std::unique_ptr<Pipeline>& pipeline,
        bool inputGeneratesScoreDetails,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) override;

    std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
        const std::vector<std::string>& pipelineNames,
        const StringMap<double>& weights,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) override;

    void groupDocsByIdAcrossInputPipelineScoreDetails(StringData pipelineName,
                                                      BSONObjBuilder& pushBob) override;

    void projectReduceInternalFieldsScoreDetails(BSONObjBuilder& bob,
                                                 StringData pipelineName,
                                                 bool forInitialValue) override;

    void constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(BSONObjBuilder& bob,
                                                                       StringData pipelineName,
                                                                       double weight) override;

    RankFusionSpec getSpec() const {
        return _spec;
    }

    int getRankConstant() const {
        return _rankConstant;
    }

public:
    RankFusionPipelineBuilder(RankFusionSpec spec, StringMap<double> weights)
        : HybridSearchPipelineBuilder(weights,
                                      kRankFusionInternalFieldsName,
                                      kRankFusionDocsFieldName,
                                      spec.getScoreDetails(),
                                      kRankFusionScoreDetailsDescription),
          _spec(spec),
          _rankConstant(kRankConstant) {}
};
}  // namespace mongo
