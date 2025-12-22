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
#pragma once
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/util/modules.h"

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

    static void assertVTableConstraints(const VTable_t& vtable) {
        sdk_tassert(11098300,
                    "QueryExecutionContext' 'check_for_interrupt' is null",
                    vtable.check_for_interrupt != nullptr);
        sdk_tassert(11213507,
                    "QueryExecutionContext' 'get_metrics' is null",
                    vtable.get_metrics != nullptr);
    };
};

using QueryExecutionContextHandle = UnownedHandle<::MongoExtensionQueryExecutionContext>;
}  // namespace sdk
}  // namespace mongo::extension
