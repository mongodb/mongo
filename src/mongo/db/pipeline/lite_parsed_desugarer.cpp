// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_desugarer.h"

#include "mongo/base/init.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"

namespace mongo {

// Register the desugarer so that ResolvedNamespace::parseViewPipeline() can desugar extension
// stages in view definitions without a direct dependency on the lite_parsed_desugarer target.
MONGO_INITIALIZER(RegisterViewPipelineDesugarer)(InitializerContext*) {
    ResolvedNamespace::setViewPipelineDesugarer(&LiteParsedDesugarer::desugar);
}

bool LiteParsedDesugarer::desugar(LiteParsedPipeline* pipeline,
                                  std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext) {
    const auto& stages = pipeline->getStages();
    bool modified = false;
    size_t i = 0;
    while (i < stages.size()) {
        tassert(11507800, "Stage pointer is null", stages[i].get());
        auto& stage = *stages[i];

        // Recursively desugar any subpipelines for this stage. If any of the subpipelines indicates
        // that the subpipeline was modified, we will need to potentially reparse the full pipeline
        // from LPP - stages with subpipelines should pass the desugared LP subpipelines through
        // StageParams.
        if (ifrContext) {
            if (auto* subpipelines = stage.getMutableSubPipelines()) {
                for (auto& subpipelineLpp : *subpipelines) {
                    modified |= LiteParsedDesugarer::desugar(&*subpipelineLpp, ifrContext);
                }
            }
        }

        // Check if the stage is desugarable by looking in the stageExpander map.
        if (auto it = _stageExpanders.find(stage.getStageParams()->getId());
            it != _stageExpanders.end()) {
            i = it->second(pipeline, i, stage);
            modified = true;
        } else {
            ++i;
        }
    }

    // Reset deferred caches since _stageSpecs may have changed.
    if (modified) {
        pipeline->resetDeferredCaches();
    }

    return modified;
}

}  // namespace mongo
