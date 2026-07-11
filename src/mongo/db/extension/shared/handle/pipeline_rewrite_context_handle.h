// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {
enum PipelineRewriteRuleTags : uint32_t {
    // Rules that optimize the internals of a stage in place but never touch adjacent stages.
    kInPlace = 1 << 0,
    // Rules that may e.g. reorder, combine or remove stages.
    kReordering = 1 << 1,
};

class PipelineRewriteRule {
public:
    PipelineRewriteRule(std::string_view name, uint32_t tags) : name(name), tags(tags) {};

    ::MongoExtensionPipelineRewriteRule convertToCRule() const {
        return {.name = {reinterpret_cast<const uint8_t*>(name.c_str()), name.size()},
                .tags = static_cast<::MongoExtensionPipelineRewriteRuleTags>(tags)};
    }

    std::string name;
    uint32_t tags;
};

using PipelineRewriteContextHandle = UnownedHandle<::MongoExtensionPipelineRewriteContext>;
using ConstPipelineRewriteContextHandle =
    Handle<const ::MongoExtensionPipelineRewriteContext, false>;

class PipelineRewriteContextAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionPipelineRewriteContext> {
    using CppApi_t = PipelineRewriteContextAPI;
};

class PipelineRewriteContextAPI : public VTableAPI<::MongoExtensionPipelineRewriteContext> {
public:
    PipelineRewriteContextAPI(::MongoExtensionPipelineRewriteContext* ptr)
        : VTableAPI<::MongoExtensionPipelineRewriteContext>(ptr) {}

    /**
     * Returns the Nth stage after the current stage in the pipeline (N=1 is the immediately
     * following stage). The returned pointer is unowned and valid only for the lifetime of the
     * context. Throws if fewer than N stages follow the current stage.
     */
    UnownedLogicalAggStageHandle getNthNextStage(size_t index) const {
        ::MongoExtensionLogicalAggStage* logicalAggStage{nullptr};
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().get_nth_next_stage(get(), index, &logicalAggStage); });
        return UnownedLogicalAggStageHandle(logicalAggStage);
    }

    /**
     * Removes the Nth stage after the current stage from the pipeline (N=1 is the immediately
     * following stage). Returns true if a stage was erased. Throws if fewer than N stages follow
     * the current stage.
     */
    bool eraseNthNext(size_t index) {
        bool result = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().erase_nth_next_stage(get(), index, &result); });
        return result;
    }

    /**
     * Returns true if at least N stages follow the current stage in the pipeline. Use this to
     * guard calls to getNthNextStage(N) or eraseNthNext(N) before invoking them.
     */
    bool hasAtLeastNNextStages(size_t n) const {
        bool result = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().has_at_least_n_next_stages(get(), n, &result); });
        return result;
    }

    /**
     * Computes and returns the DocsNeededBounds for all stages in the pipeline after the current
     * stage.
     */
    MongoExtensionDocsNeededBounds getPipelineSuffixBounds() const {
        MongoExtensionDocsNeededBounds result{};

        invokeCAndConvertStatusToException(
            [&]() { return _vtable().get_pipeline_suffix_bounds(get(), &result); });
        return result;
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(12200600,
                "PipelineRewriteContextAPI 'get_nth_next_stage' is null",
                vtable.get_nth_next_stage != nullptr);
        tassert(12200601,
                "PipelineRewriteContextAPI 'erase_nth_next_stage' is null",
                vtable.erase_nth_next_stage != nullptr);
        tassert(12200607,
                "PipelineRewriteContextAPI 'has_at_least_n_next_stages' is null",
                vtable.has_at_least_n_next_stages != nullptr);
        tassert(12200501,
                "PipelineRewriteContextAPI 'get_pipeline_suffix_bounds' is null",
                vtable.get_pipeline_suffix_bounds != nullptr);
    }
};

}  // namespace mongo::extension
