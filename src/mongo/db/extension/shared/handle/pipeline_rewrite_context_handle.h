/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
    PipelineRewriteRule(std::string name, uint32_t tags) : name(std::move(name)), tags(tags) {};

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
    }
};

}  // namespace mongo::extension
