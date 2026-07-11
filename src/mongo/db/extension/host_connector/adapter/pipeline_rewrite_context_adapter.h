// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host/pipeline_rewrite_context.h"
#include "mongo/db/extension/host_connector/adapter/logical_agg_stage_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {

class PipelineRewriteContextAdapter final : public ::MongoExtensionPipelineRewriteContext {
public:
    PipelineRewriteContextAdapter(std::unique_ptr<host::PipelineRewriteContext> ctx)
        : ::MongoExtensionPipelineRewriteContext(&VTABLE), _ctx(std::move(ctx)) {
        tassert(12200604,
                "The adapter's underlying host pipeline rewrite context is invalid.",
                _ctx != nullptr);
    }

    ~PipelineRewriteContextAdapter() = default;

    // PipelineRewriteContextAdapter is non-copyable and non-moveable, as adapters should be
    // heap allocated, and managed via a unique_ptr or Handle. This property guarantees that the
    // adapter's underlying implementation pointer remains valid for object's lifetime.
    PipelineRewriteContextAdapter(const PipelineRewriteContextAdapter&) = delete;
    PipelineRewriteContextAdapter& operator=(const PipelineRewriteContextAdapter&) = delete;
    PipelineRewriteContextAdapter(PipelineRewriteContextAdapter&&) = delete;
    PipelineRewriteContextAdapter& operator=(PipelineRewriteContextAdapter&&) = delete;


    host::PipelineRewriteContext& getCtxImpl() {
        tassert(12200605, "PipelineRewriteContextAdapter has no context", _ctx != nullptr);
        return *_ctx;
    }

    const host::PipelineRewriteContext& getCtxImpl() const {
        tassert(12200606, "PipelineRewriteContextAdapter has no context", _ctx != nullptr);
        return *_ctx;
    }

private:
    static MongoExtensionStatus* _hostGetNthNextStage(
        const MongoExtensionPipelineRewriteContext* extCtx,
        size_t index,
        MongoExtensionLogicalAggStage** out);
    static MongoExtensionStatus* _hostEraseNthNextStage(
        MongoExtensionPipelineRewriteContext* extCtx, size_t index, bool* result);
    static MongoExtensionStatus* _hostHasAtLeastNNextStages(
        const MongoExtensionPipelineRewriteContext* extCtx, size_t n, bool* result);
    static MongoExtensionStatus* _hostGetPipelineSuffixBounds(
        const MongoExtensionPipelineRewriteContext* extCtx, MongoExtensionDocsNeededBounds* out);

    static constexpr ::MongoExtensionPipelineRewriteContextVTable VTABLE = {
        .get_nth_next_stage = &_hostGetNthNextStage,
        .erase_nth_next_stage = &_hostEraseNthNextStage,
        .has_at_least_n_next_stages = &_hostHasAtLeastNNextStages,
        .get_pipeline_suffix_bounds = &_hostGetPipelineSuffixBounds,
    };

    std::unique_ptr<host::PipelineRewriteContext> _ctx;
    // The resulting stage of _hostGetNthNextStage (the Nth next stage) is cached and ultimately
    // replaced during the next invocation of _hostGetNthNextStage. Note that this field is mutable
    // because conceptually the PipelineRewriteContextAdapter should be const because its underlying
    // logical state (the RBR PipelineRewriteContext) is unchanged when the precondition is
    // evaluated. This field is part of the observable state and must be updated for correctness.
    mutable std::unique_ptr<HostLogicalAggStageAdapter> cachedNextStageResult;
};

/**
 * Wraps an extension PipelineRewriteRule into a host-side PipelineRewriteRule by adapting the
 * extension's evaluatePipelineRewriteRulePrecondition/evaluatePipelineRewriteRuleTransform vtable
 * calls into the host's rule-based rewriter precondition/transform function signatures.
 */
rule_based_rewrites::pipeline::PipelineRewriteRule wrapExtensionRule(
    const extension::PipelineRewriteRule& extRule, UnownedLogicalAggStageHandle extensionStage);

}  // namespace mongo::extension::host_connector
