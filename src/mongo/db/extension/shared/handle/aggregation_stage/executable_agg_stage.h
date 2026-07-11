// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension {

class ExecAggStageAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionExecAggStage> {
    using CppApi_t = ExecAggStageAPI;
};

using ExecAggStageHandle = OwnedHandle<::MongoExtensionExecAggStage>;
using UnownedExecAggStageHandle = UnownedHandle<::MongoExtensionExecAggStage>;

/**
 * ExecAggStageHandle is a wrapper around a MongoExtensionExecAggStage.
 */
class ExecAggStageAPI : public VTableAPI<::MongoExtensionExecAggStage> {
public:
    ExecAggStageAPI(::MongoExtensionExecAggStage* execAggStage)
        : VTableAPI<::MongoExtensionExecAggStage>(execAggStage) {}

    ExtensionGetNextResult getNext(MongoExtensionQueryExecutionContext* execCtxPtr);

    std::string_view getName() const;

    OwnedOperationMetricsHandle createMetrics() const;

    void setSource(const ExecAggStageHandle& input);

    void open();

    void reopen();

    void close();

    /**
     * Collects explain output at the specified verbosity from this executable stage.
     */
    BSONObj explain(MongoExtensionQueryExecutionContext& execCtx,
                    ExplainOptions::Verbosity verbosity) const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'get_next' is null",
                vtable.get_next != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'create_metrics' is null",
                vtable.create_metrics != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'set_source' is null",
                vtable.set_source != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'open' is null",
                vtable.open != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'reopen' is null",
                vtable.reopen != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'close' is null",
                vtable.close != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExecAggStage 'explain' is null",
                vtable.explain != nullptr);
    }
};
}  // namespace mongo::extension
