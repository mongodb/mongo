// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/base/init.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Registers a function that desugars a lite parsed pipeline stage.
 * 'registrationName': unique name to give to the initializer function that does the registration.
 * 'stageParamsId': unique StageParams::Id assigned to the StageParams class.
 * 'stageExpander': function that expands a pipeline stage in place and returns the index one past
 * the last stage it inserted.
 */
#define REGISTER_LITE_PARSED_DESUGARER_STAGE_EXPANDER(                                     \
    registrationName, stageParamsId, stageExpander)                                        \
    namespace {                                                                            \
    MONGO_INITIALIZER_GENERAL(registerLiteParsedDesugarerStageExpander_##registrationName, \
                              ("BeginLiteParsedDesugarerStageExpanderRegistration"),       \
                              ("EndLiteParsedDesugarerStageExpanderRegistration"))         \
    (InitializerContext*) {                                                                \
        LiteParsedDesugarer::registerStageExpander(                                        \
            stageParamsId, stageExpander, #registrationName);                              \
    }                                                                                      \
    }

class [[MONGO_MOD_PUBLIC]] LiteParsedDesugarer {
public:
    using StageExpander =
        std::function<size_t(LiteParsedPipeline*, size_t index, LiteParsedDocumentSource&)>;

    /**
     * Desugars the LiteParsedPipeline and returns whether the pipeline was modified or not. Callers
     * that embed a sub-pipeline (e.g. $unionWith) should call desugar() eagerly on the sub-pipeline
     * so that extension sources are replaced by their expanded LiteParsed representation before
     * bindResolvedNamespace() and constraints checks run.
     */
    static bool desugar(LiteParsedPipeline* pipeline,
                        std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext);

    /**
     * Registers a stage expander for the desugarer.
     * Do not call this function directly. Instead, use the
     * 'REGISTER_LITE_PARSED_DESUGARER_STAGE_EXPANDER' macro defined in this file, which ensures
     * registration runs in the correct initialization phase.
     */
    static void registerStageExpander(StageParams::Id id,
                                      StageExpander stageExpander,
                                      std::string_view name);

private:
    // Associate a stage expander for each stage that should desugar.
    // NOTE: this map is *not* thread safe. LiteParsedDocumentSources should register their
    // stageExpander using 'REGISTER_LITE_PARSED_DESUGARER_STAGE_EXPANDER' to ensure thread safety.
    // See DocumentSourceExtensionOptimizable::LiteParsedExpandable for an example.
    inline static stdx::unordered_map<StageParams::Id, StageExpander> _stageExpanders{};
};

}  // namespace mongo
