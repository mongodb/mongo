// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension {
namespace sdk {
class QueryExecutionContextAPI;
}
template <>
struct c_api_to_cpp_api<::MongoExtensionQueryExecutionContext> {
    using CppApi_t = sdk::QueryExecutionContextAPI;
};

namespace sdk {
/**
 * Wrapper for ::MongoExtensionQueryExecutionContext, providing safe access to its public API
 * through the underlying vtable.
 *
 * Typically ownership of MongoExtensionQueryExecutionContext pointer is never transferred to the
 * extension, so this API is expected to only be used with an UnownedHandle.
 */
class QueryExecutionContextAPI : public VTableAPI<::MongoExtensionQueryExecutionContext> {
public:
    QueryExecutionContextAPI(::MongoExtensionQueryExecutionContext* ctx)
        : VTableAPI<::MongoExtensionQueryExecutionContext>(ctx) {}

    ExtensionGenericStatus checkForInterrupt() const;

    UnownedOperationMetricsHandle getMetrics(MongoExtensionExecAggStage* execStage) const;

    int64_t getDeadlineTimestampMs() const;

    /**
     * Returns a BSONObj containing the requested host metrics from OpDebug. Throws if any metric
     * name is not recognized. Metrics that are valid but not yet populated are omitted from the
     * result.
     */
    BSONObj getHostMetrics(const std::vector<std::string_view>& metricNames) const;

    void setBatchSize(uint64_t batchSize) const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        sdk_tassert(11098300,
                    "QueryExecutionContext' 'check_for_interrupt' is null",
                    vtable.check_for_interrupt != nullptr);
        sdk_tassert(11213507,
                    "QueryExecutionContext' 'get_metrics' is null",
                    vtable.get_metrics != nullptr);
        sdk_tassert(11646100,
                    "QueryExecutionContext' 'get_deadline_timestamp_ms' is null",
                    vtable.get_deadline_timestamp_ms != nullptr);
        sdk_tassert(12199900,
                    "QueryExecutionContext' 'get_host_metrics' is null",
                    vtable.get_host_metrics != nullptr);
        sdk_tassert(13150703,
                    "QueryExecutionContext' 'set_batch_size' is null",
                    vtable.set_batch_size != nullptr);
    };
};

using QueryExecutionContextHandle = UnownedHandle<::MongoExtensionQueryExecutionContext>;
}  // namespace sdk
}  // namespace mongo::extension
