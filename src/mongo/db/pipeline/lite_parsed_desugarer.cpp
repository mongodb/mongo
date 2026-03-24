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

#include "mongo/db/pipeline/lite_parsed_desugarer.h"

#include "mongo/base/init.h"
#include "mongo/db/pipeline/resolved_namespace.h"
// TODO SERVER-120179 Remove when the feature flag is enabled.
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo {

// Register the desugarer so that ResolvedNamespace::parseViewPipeline() can desugar extension
// stages in view definitions without a direct dependency on the lite_parsed_desugarer target.
MONGO_INITIALIZER(RegisterViewPipelineDesugarer)(InitializerContext*) {
    ResolvedNamespace::setViewPipelineDesugarer(&LiteParsedDesugarer::desugar);
}

bool LiteParsedDesugarer::desugar(LiteParsedPipeline* pipeline) {
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
        // TODO SERVER-120179 Remove the feature flag guard when the feature flag is enabled.
        if (feature_flags::gFeatureFlagExtensionViewsAndUnionWith.isEnabled()) {
            auto& subpipelines = stage.getMutableSubPipelines();
            for (auto& subpipelineLpp : subpipelines) {
                modified |= LiteParsedDesugarer::desugar(&subpipelineLpp);
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
