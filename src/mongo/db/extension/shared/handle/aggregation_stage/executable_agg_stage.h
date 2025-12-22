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

#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

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
    BSONObj explain(ExplainOptions::Verbosity verbosity) const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(10956800, "ExecAggStage 'get_next' is null", vtable.get_next != nullptr);
        tassert(11213503, "ExecAggStage 'get_name' is null", vtable.get_name != nullptr);
        tassert(
            11213504, "ExecAggStage 'create_metrics' is null", vtable.create_metrics != nullptr);
        tassert(10957202, "ExecAggStage 'set_source' is null", vtable.set_source != nullptr);
        tassert(11216705, "ExecAggStage 'open' is null", vtable.open != nullptr);
        tassert(11216706, "ExecAggStage 'reopen' is null", vtable.reopen != nullptr);
        tassert(11216707, "ExecAggStage 'close' is null", vtable.close != nullptr);
    }
};
}  // namespace mongo::extension
