// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExpressionContext;
class Pipeline;

/**
 * Stage params produced by LiteParsedRankFusion and consumed by
 * DocumentSourceRankFusion::createFromStageParams.
 */
class RankFusionStageParams : public StageParams {
public:
    RankFusionStageParams(RankFusionSpec spec,
                          const std::vector<OwnedLiteParsedPipeline>& pipelines,
                          BSONObj originalBson)
        : _spec(std::move(spec)), _originalBson(std::move(originalBson)) {
        _pipelines.reserve(pipelines.size());
        for (const auto& ownedPipeline : pipelines) {
            _pipelines.push_back(ownedPipeline->clone());
        }
    }

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const RankFusionSpec& getSpec() const {
        return _spec;
    }
    const std::vector<LiteParsedPipeline>& getPipelines() const {
        return _pipelines;
    }
    BSONElement getOriginalBson() const {
        return _originalBson.firstElement();
    }

    /**
     * Builds the input pipelines map from the already-validated LiteParsedPipelines stored on
     * this object. Used by DocumentSourceRankFusion::createFromStageParams to avoid re-parsing
     * the IDL.
     */
    std::map<std::string, std::unique_ptr<Pipeline>> buildInputPipelines(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx) const;

private:
    RankFusionSpec _spec;
    std::vector<LiteParsedPipeline> _pipelines;
    BSONObj _originalBson;
};

/**
 * Lite-parsed representation of the $rankFusion stage.
 */
class LiteParsedRankFusion final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedRankFusion> {
public:
    static std::unique_ptr<LiteParsedRankFusion> parse(const NamespaceString& nss,
                                                       const BSONElement& spec,
                                                       const LiteParserOptions& options);

    LiteParsedRankFusion(const BSONElement& spec,
                         const NamespaceString& nss,
                         RankFusionSpec parsedSpec,
                         std::vector<OwnedLiteParsedPipeline> pipelines,
                         bool extensionsInHybridSearchEnabled = false)
        : LiteParsedDocumentSourceNestedPipelines(spec, nss, std::move(pipelines)),
          _parsedSpec(std::move(parsedSpec)),
          _extensionsInHybridSearchEnabled(extensionsInHybridSearchEnabled) {}

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool isSearchStage() const final {
        return !_pipelines.empty() && _pipelines[0]->hasSearchStage();
    }

    // Suppress recursive subpipeline view resolution: views are stitched once on the desugared
    // $unionWith stages, and applying them to '_pipelines[]' too would duplicate view stages.
    // TODO SERVER-121094 Remove once the flag is gone and desugaring is unconditional.
    bool shouldResolveSubpipelineViews() const final {
        return false;
    }

    bool isHybridSearchStage() const final {
        return true;
    }

    // $rankFusion desugars into a pipeline that includes $sort.
    bool isRankedStage() const final {
        return true;
    }

    // $rankFusion computes a reciprocal rank fusion score for each document.
    bool isScoredStage() const final {
        return true;
    }

    // $rankFusion produces scoreDetails metadata when the user requests it via the spec.
    bool isScoreDetailsStage() const final {
        return _parsedSpec.getScoreDetails();
    }

    // $rankFusion does not modify documents, only combines and reorders them.
    bool isSelectionStage() const final {
        return true;
    }

    Constraints constraints() const override {
        return {.canRunOnTimeseries = false};
    }

    void validate() const override;

    bool hasExtensionVectorSearchStage() const override {
        return std::any_of(_pipelines.begin(), _pipelines.end(), [](const auto& ownedPipeline) {
            return ownedPipeline->hasExtensionVectorSearchStage();
        });
    }

    bool hasExtensionSearchStage() const override {
        return std::any_of(_pipelines.begin(), _pipelines.end(), [](const auto& ownedPipeline) {
            return ownedPipeline->hasExtensionSearchStage();
        });
    }

    std::unique_ptr<StageParams> getStageParams() const override {
        return std::make_unique<RankFusionStageParams>(
            _parsedSpec, _pipelines, getOriginalBson().wrap().getOwned());
    }

    const RankFusionSpec& getSpec() const {
        return _parsedSpec;
    }

    bool extensionsInHybridSearchEnabled() const {
        return _extensionsInHybridSearchEnabled;
    }

private:
    RankFusionSpec _parsedSpec;
    // True when, at parse time, the IFR context reports featureFlagExtensionsInsideHybridSearch is
    // enabled. Used by rankFusionStageExpander to decide whether to desugar at lite-parse time.
    // TODO SERVER-121094 Remove this field (and extensionsInHybridSearchEnabled()) once the flag is
    // removed and lite-parse desugaring is unconditional.
    bool _extensionsInHybridSearchEnabled = false;
};

}  // namespace mongo
