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

#include "mongo/db/extension/host/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/host/aggregation_stage/logical_agg_stage.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {

/**
 * Boundary object representation of a ::MongoExtensionLogicalAggStage.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionLogicalAggStage interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the LogicalAggStage.
 */
class HostLogicalAggStageAdapter final : public ::MongoExtensionLogicalAggStage {
public:
    HostLogicalAggStageAdapter(std::unique_ptr<host::LogicalAggStage> logicalAggStage)
        : ::MongoExtensionLogicalAggStage(&VTABLE), _logicalAggStage(std::move(logicalAggStage)) {
        tassert(12303709,
                "The adapter's underlying host logical stage is invalid.",
                _logicalAggStage != nullptr);
    }

    ~HostLogicalAggStageAdapter() = default;

    // HostLogicalAggStageAdapter is non-copyable and non-moveable, as adapters should be heap
    // allocated, and managed via a unique_ptr or Handle.
    // This property guarantees that the adapter's underlying implementation pointer remains valid
    // for object's lifetime.
    HostLogicalAggStageAdapter(const HostLogicalAggStageAdapter&) = delete;
    HostLogicalAggStageAdapter& operator=(const HostLogicalAggStageAdapter&) = delete;
    HostLogicalAggStageAdapter(HostLogicalAggStageAdapter&&) = delete;
    HostLogicalAggStageAdapter& operator=(HostLogicalAggStageAdapter&&) = delete;

    /**
     * Specifies whether the provided logical agg stage was allocated by the host.
     *
     * Since ExtensionLogicalAggStageAdapter and HostLogicalAggStageAdapter implement the same
     * vtable, this function is necessary for differentiating between host-allocated and
     * extension-allocated logical agg stages.
     *
     * Use this function to check if a logical agg stage is host-allocated before casting a
     * MongoExtensionLogicalAggStage to a HostLogicalAggStageAdapter.
     */
    static inline bool isHostAllocated(::MongoExtensionLogicalAggStage& logicalAggStage) {
        return logicalAggStage.vtable == &VTABLE;
    }

private:
    const host::LogicalAggStage& getImpl() const noexcept {
        return *_logicalAggStage;
    }

    host::LogicalAggStage& getImpl() noexcept {
        return *_logicalAggStage;
    }

    static void _hostDestroy(::MongoExtensionLogicalAggStage* logicalStage) noexcept {
        delete static_cast<HostLogicalAggStageAdapter*>(logicalStage);
    }
    static ::MongoExtensionByteView _hostGetName(
        const ::MongoExtensionLogicalAggStage* logicalStage) noexcept;

    static ::MongoExtensionStatus* _hostSerialize(
        const ::MongoExtensionLogicalAggStage* logicalStage,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303700,
                      "_hostSerialize should not be called on host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostExplain(const ::MongoExtensionLogicalAggStage* logicalStage,
                                                ::MongoExtensionQueryExecutionContext* execCtxPtr,
                                                ::MongoExtensionExplainVerbosity verbosity,
                                                ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303701,
                      "_hostExplain should not be called on a host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostCompile(const ::MongoExtensionLogicalAggStage* logicalStage,
                                                ::MongoExtensionExecAggStage** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303702,
                      "_hostCompile should not be called on a host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostGetDistributedPlanLogic(
        const ::MongoExtensionLogicalAggStage* logicalStage,
        ::MongoExtensionDistributedPlanLogic** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303703,
                      "_hostGetDistributedPlanLogic should not be called on a host-allocated "
                      "logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostClone(const ::MongoExtensionLogicalAggStage* logicalStage,
                                              ::MongoExtensionLogicalAggStage** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303704,
                      "_hostClone should not be called on a host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostIsStageSortedByVectorSearchScore(
        const ::MongoExtensionLogicalAggStage* logicalStage,
        bool* outIsSortedByVectorSearchScore) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303705,
                      "_hostIsStageSortedByVectorSearchScore should not be called on a "
                      "host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostSetVectorSearchLimitForOptimization(
        ::MongoExtensionLogicalAggStage* logicalStage, long long* extractedLimitVal) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303706,
                      "_hostSetVectorSearchLimitForOptimization should not be called on a "
                      "host-allocated logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostEvaluateRulePrecondition(
        const ::MongoExtensionLogicalAggStage* logicalStage,
        ::MongoExtensionByteView ruleName,
        const ::MongoExtensionPipelineRewriteContext* ctx,
        bool* result) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303707,
                      "_hostEvaluateRulePrecondition should not be called on a host-allocated "
                      "logical stage.");
        });
    }

    static ::MongoExtensionStatus* _hostEvaluateRuleTransform(
        ::MongoExtensionLogicalAggStage* logicalStage,
        ::MongoExtensionByteView ruleName,
        ::MongoExtensionPipelineRewriteContext* ctx,
        bool* result) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12303708,
                      "_hostEvaluateRuleTransform should not be called on a host-allocated logical "
                      "stage.");
        });
    }

    static ::MongoExtensionStatus* _hostGetFilter(
        const ::MongoExtensionLogicalAggStage* logicalStage,
        ::MongoExtensionByteBuf** output) noexcept;

    static ::MongoExtensionStatus* _hostApplyPipelineSuffixDependencies(
        ::MongoExtensionLogicalAggStage* logicalStage,
        const ::MongoExtensionPipelineDependencies* deps) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12200104,
                      "_hostApplyPipelineSuffixDependencies should not be called on a "
                      "host-allocated logical stage.");
        });
    }

    static constexpr ::MongoExtensionLogicalAggStageVTable VTABLE = {
        .destroy = &_hostDestroy,
        .get_name = &_hostGetName,
        .serialize = &_hostSerialize,
        .explain = &_hostExplain,
        .compile = &_hostCompile,
        .get_distributed_plan_logic = &_hostGetDistributedPlanLogic,
        .clone = &_hostClone,
        .is_stage_sorted_by_vector_search_score_deprecated = &_hostIsStageSortedByVectorSearchScore,
        .set_vector_search_limit_for_optimization_deprecated =
            &_hostSetVectorSearchLimitForOptimization,
        .evaluate_rule_precondition = &_hostEvaluateRulePrecondition,
        .evaluate_rule_transform = &_hostEvaluateRuleTransform,
        .get_filter = &_hostGetFilter,
        .apply_pipeline_suffix_dependencies = &_hostApplyPipelineSuffixDependencies,
    };

    std::unique_ptr<host::LogicalAggStage> _logicalAggStage;
};

};  // namespace mongo::extension::host_connector
