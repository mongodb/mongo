// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"

namespace mongo {

OwnedLiteParsedPipeline::OwnedLiteParsedPipeline(const NamespaceString& nss,
                                                 const std::vector<BSONObj>& pipelineStages,
                                                 const LiteParserOptions& options)
    : _ownedStages(_makeStagesOwned(pipelineStages)),
      // Subpipelines are never the top-level view pipeline; pass false for the hybrid-search flag.
      _pipeline(nss, _ownedStages, /*isRunningAgainstView_ForHybridSearch=*/false, options) {}

OwnedLiteParsedPipeline::OwnedLiteParsedPipeline(NamespaceString nss, StageSpecs stages)
    // No new BSON is parsed here, so there is nothing for _ownedStages to hold. The moved-in
    // stages may still hold unowned BSONElement views into transient buffers (e.g. extension
    // SDK objects destroyed once expand() returns), so force each stage to self-own its BSON now.
    : _pipeline(std::move(nss), std::move(stages)) {
    _pipeline.makeOwned();
}

OwnedLiteParsedPipeline::OwnedLiteParsedPipeline(const OwnedLiteParsedPipeline& other)
    // Each stage in the copy receives a fresh, independently-owned BSONObj: the pipeline is
    // cloned (copying each stage's parsed state), then makeOwned() calls _originalBson.wrap()
    // on each stage to produce a new self-sufficient BSONObj — the copy-ctor equivalent of
    // the primary ctor's _makeStagesOwned() pass. _ownedStages is left empty because each
    // stage carries its own BSON directly after makeOwned().
    : _ownedStages(), _pipeline(other._pipeline), _viewNss(other._viewNss) {
    _pipeline.makeOwned();
}

// static
std::vector<BSONObj> OwnedLiteParsedPipeline::_makeStagesOwned(
    const std::vector<BSONObj>& pipelineStages) {
    std::vector<BSONObj> owned;
    owned.reserve(pipelineStages.size());
    for (const auto& stage : pipelineStages) {
        owned.push_back(stage.getOwned());
    }
    return owned;
}

}  // namespace mongo
