// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"

#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

#include <string_view>

namespace mongo::extension {

std::string_view LogicalAggStageAPI::getName() const {
    auto stringView = byteViewAsStringView(_vtable().get_name(get()));
    return std::string_view{stringView.data(), stringView.size()};
}

BSONObj LogicalAggStageAPI::serialize() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().serialize(get(), &buf); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "Extension implementation of `serialize` encountered nullptr inside the output buffer.",
            buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the caller.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

BSONObj LogicalAggStageAPI::explain(MongoExtensionQueryExecutionContext& execCtx,
                                    mongo::ExplainOptions::Verbosity verbosity) const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() {
        return _vtable().explain(
            get(), &execCtx, convertHostVerbosityToExtVerbosity(verbosity), &buf);
    });

    tassert(ErrorCodes::ExtensionSerializationError,
            "buffer returned from explain must not be null",
            buf);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

ExecAggStageHandle LogicalAggStageAPI::compile() const {
    ::MongoExtensionExecAggStage* execAggStage{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().compile(get(), &execAggStage); });

    return ExecAggStageHandle(execAggStage);
}

DistributedPlanLogicHandle LogicalAggStageAPI::getDistributedPlanLogic() const {
    ::MongoExtensionDistributedPlanLogic* dpl{nullptr};
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_distributed_plan_logic(get(), &dpl); });

    return DistributedPlanLogicHandle(dpl);
}

LogicalAggStageHandle LogicalAggStageAPI::clone() const {
    ::MongoExtensionLogicalAggStage* logicalAggStage{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().clone(get(), &logicalAggStage); });

    return LogicalAggStageHandle(logicalAggStage);
}

void LogicalAggStageAPI::setExtractedLimitVal_deprecated(
    boost::optional<long long> extractedLimitVal) {
    invokeCAndConvertStatusToException([&]() {
        if (extractedLimitVal.has_value()) {
            return _vtable().set_vector_search_limit_for_optimization_deprecated(
                get(), &extractedLimitVal.get());
        } else {
            return _vtable().set_vector_search_limit_for_optimization_deprecated(get(), nullptr);
        }
    });
}

bool LogicalAggStageAPI::evaluatePipelineRewriteRulePrecondition(
    std::string_view ruleName, MongoExtensionPipelineRewriteContext* pipelineRewriteContext) const {
    bool result = false;
    auto nameView = ::MongoExtensionByteView{reinterpret_cast<const uint8_t*>(ruleName.data()),
                                             ruleName.size()};
    invokeCAndConvertStatusToException([&]() {
        return _vtable().evaluate_pipeline_rewrite_rule_precondition(
            get(), nameView, pipelineRewriteContext, &result);
    });
    return result;
}

bool LogicalAggStageAPI::evaluatePipelineRewriteRuleTransform(
    std::string_view ruleName, MongoExtensionPipelineRewriteContext* pipelineRewriteContext) {
    bool result = false;
    auto nameView = ::MongoExtensionByteView{reinterpret_cast<const uint8_t*>(ruleName.data()),
                                             ruleName.size()};
    invokeCAndConvertStatusToException([&]() {
        return _vtable().evaluate_pipeline_rewrite_rule_transform(
            get(), nameView, pipelineRewriteContext, &result);
    });
    return result;
}

BSONObj LogicalAggStageAPI::getFilter() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().get_filter(get(), &buf); });

    if (!buf) {
        // 'buf' will be null if the logical stage does not have a filter.
        return BSONObj();
    }

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

void LogicalAggStageAPI::applyPipelineSuffixDependencies(
    const ::MongoExtensionPipelineDependencies* deps) {
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().apply_pipeline_suffix_dependencies(get(), deps); });
}


void LogicalAggStageAPI::skipStream(::MongoExtensionStreamType streamType) {
    invokeCAndConvertStatusToException([&]() { return _vtable().skip_stream(get(), streamType); });
}

/**
 * Returns the sort pattern applied by this stage. Returns an empty BSONObj if the stage does
 * not apply a sort pattern.
 */
BSONObj LogicalAggStageAPI::getSortPattern() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().get_sort_pattern(get(), &buf); });

    if (!buf) {
        return BSONObj();
    }

    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

boost::optional<MongoExtensionDocsNeededBoundsInfo> LogicalAggStageAPI::getDocsNeededBounds()
    const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_docs_needed_bounds(get(), &buf); });

    if (!buf) {
        return boost::none;
    }

    ExtensionByteBufHandle ownedBuf{buf};
    auto bson = bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
    return MongoExtensionDocsNeededBoundsInfo::parse(bson);
}

}  // namespace mongo::extension
