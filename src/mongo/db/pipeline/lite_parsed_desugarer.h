// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] LiteParsedDesugarer {
public:
    using StageExpander =
        std::function<size_t(LiteParsedPipeline*, size_t index, LiteParsedDocumentSource&)>;

    // Desugars the LiteParsedPipeline and returns whether the pipeline was modified or not. Callers
    // that embed a sub-pipeline (e.g. $unionWith) should call desugar() eagerly on the sub-pipeline
    // so that extension sources are replaced by their expanded LiteParsed representation before
    // bindResolvedNamespace() and constraints checks run.
    static bool desugar(LiteParsedPipeline* pipeline,
                        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext);

    static void registerStageExpander(StageParams::Id id, StageExpander stageExpander) {
        _stageExpanders[id] = std::move(stageExpander);
    }

private:
    // Associate a stage expander for each stage that should desugar.
    // NOTE: this map is *not* thread safe. LiteParsedDocumentSources should register their
    // stageExpander using MONGO_INITIALIZER to ensure thread safety. See
    // DocumentSourceExtensionOptimizable::LiteParsedExpandable for an example.
    inline static stdx::unordered_map<StageParams::Id, StageExpander> _stageExpanders{};
};

}  // namespace mongo
