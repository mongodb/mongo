// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/hybrid_search_pipeline_builder.h"
#include "mongo/util/modules.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This ScoreFusionPipelineBuilder class stores the builder methods to build the desugared stages
 * for any given $scoreFusion input pipeline and the final scoring and merging logic.
 */
class [[MONGO_MOD_PRIVATE]] ScoreFusionPipelineBuilder final : public HybridSearchPipelineBuilder {
    // DocumentSourceScoreFusion::createFromBson() creates an instance of this class and calls its
    // inherited constructDesugaredOutput(...) method which calls the derived class' overriden
    // virtual methods to construct the final desugared output.
public:
    // Name of single top-level field object used to track all internal fields we need
    // intermediate to the desugar.
    // One field object that holds all internal intermediate variables during desugar,
    // like each input pipeline's individual score or scoreDetails.
    static constexpr std::string_view kScoreFusionInternalFieldsName =
        "_internal_scoreFusion_internal_fields"sv;

    // One field object to encapsulate the unmodified user's doc from the queried collection.
    static constexpr std::string_view kScoreFusionDocsFieldName = "_internal_scoreFusion_docs"sv;

    // Description that gets set as part of $scoreFusion's scoreDetails metadata.
    static constexpr std::string_view kScoreFusionScoreDetailsDescription =
        "the value calculated by combining the scores (either normalized or raw) across "
        "input pipelines from which this document is output from:";

private:
    ScoreFusionSpec _spec;

    std::list<boost::intrusive_ptr<DocumentSource>> buildInputPipelineDesugaringStages(
        std::string_view firstInputPipelineName,
        double weight,
        const std::unique_ptr<Pipeline>& pipeline,
        bool inputGeneratesScoreDetails,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) override;

    std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
        const std::vector<std::string>& pipelineNames,
        const StringMap<double>& weights,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) override;

    std::string getScoreDetailsScalarFieldName(std::string_view pipelineName) const override;

    void constructCalculatedFinalScoreDetailsStageSpecificScoreDetails(
        BSONObjBuilder& bob, std::string_view pipelineName, double weight) override;

    ScoreFusionSpec getSpec() const {
        return _spec;
    }

public:
    ScoreFusionPipelineBuilder(ScoreFusionSpec spec, StringMap<double> weights)
        : HybridSearchPipelineBuilder(weights,
                                      kScoreFusionInternalFieldsName,
                                      kScoreFusionDocsFieldName,
                                      spec.getScoreDetails(),
                                      kScoreFusionScoreDetailsDescription),
          _spec(spec) {}
};
}  // namespace mongo
