// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
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
 * Stage params produced by LiteParsedScoreFusion and consumed by
 * DocumentSourceScoreFusion::createFromStageParams.
 */
class ScoreFusionStageParams : public StageParams {
public:
    ScoreFusionStageParams(ScoreFusionSpec spec,
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

    const ScoreFusionSpec& getSpec() const {
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
     * this object. Used by DocumentSourceScoreFusion::createFromStageParams to avoid re-parsing
     * the IDL.
     */
    std::map<std::string, std::unique_ptr<Pipeline>> buildInputPipelines(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx) const;

private:
    ScoreFusionSpec _spec;
    std::vector<LiteParsedPipeline> _pipelines;
    BSONObj _originalBson;
};

/**
 * Lite-parsed representation of the $scoreFusion stage.
 */
class LiteParsedScoreFusion final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedScoreFusion> {
public:
    static std::unique_ptr<LiteParsedScoreFusion> parse(const NamespaceString& nss,
                                                        const BSONElement& spec,
                                                        const LiteParserOptions& options);

    LiteParsedScoreFusion(const BSONElement& spec,
                          const NamespaceString& nss,
                          ScoreFusionSpec parsedSpec,
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

    bool isHybridSearchStage() const final {
        return true;
    }

    // Suppress recursive subpipeline view resolution. The desugar splices the first input
    // pipeline directly into the outer pipeline and wraps the others in $unionWith, so applying
    // the view to '_pipelines[]' here would produce duplicate view stages in the outer pipeline.
    // TODO SERVER-121094 Remove once the flag is gone and desugaring is unconditional.
    bool shouldResolveSubpipelineViews() const final {
        return false;
    }

    // $scoreFusion desugars into a pipeline that includes $sort.
    bool isRankedStage() const final {
        return true;
    }

    // $scoreFusion computes a combined score for each document from its input pipelines.
    bool isScoredStage() const final {
        return true;
    }

    // $scoreFusion produces scoreDetails metadata when the user requests it via the spec.
    bool isScoreDetailsStage() const final {
        return _parsedSpec.getScoreDetails();
    }

    // $scoreFusion does not modify documents, only combines and reorders them.
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
        return std::make_unique<ScoreFusionStageParams>(
            _parsedSpec, _pipelines, getOriginalBson().wrap().getOwned());
    }

    const ScoreFusionSpec& getSpec() const {
        return _parsedSpec;
    }

    bool extensionsInHybridSearchEnabled() const {
        return _extensionsInHybridSearchEnabled;
    }

private:
    ScoreFusionSpec _parsedSpec;
    // True when, at parse time, the IFR context reports featureFlagExtensionsInsideHybridSearch is
    // enabled. Used by scoreFusionStageExpander to decide whether to desugar at lite-parse time.
    // TODO SERVER-121094 Remove this field (and extensionsInHybridSearchEnabled()) once the flag is
    // removed and lite-parse desugaring is unconditional.
    bool _extensionsInHybridSearchEnabled = false;
};

}  // namespace mongo
