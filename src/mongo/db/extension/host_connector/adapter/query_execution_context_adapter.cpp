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
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"

#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host_connector {

MongoExtensionStatus* QueryExecutionContextAdapter::_extCheckForInterrupt(
    const MongoExtensionQueryExecutionContext* ctx, MongoExtensionStatus* queryStatus) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();
        Status interrupted = execCtx.checkForInterrupt();
        // Ensure output query status is valid before accessing it.
        if (!interrupted.isOK()) {
            StatusHandle::assertValidStatus(queryStatus);
            MongoExtensionByteView reasonByteView{stringViewAsByteView(interrupted.reason())};
            // Note that we don't need invokeCAndConvertStatusToException here because
            // set_code does not throw errors.
            queryStatus->vtable->set_code(queryStatus, interrupted.code());
            invokeCAndConvertStatusToException(
                [&]() { return queryStatus->vtable->set_reason(queryStatus, reasonByteView); });
        }
    });
}

MongoExtensionStatus* QueryExecutionContextAdapter::_extGetMetrics(
    const MongoExtensionQueryExecutionContext* ctx,
    MongoExtensionExecAggStage* execAggStage,
    MongoExtensionOperationMetrics** metrics) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const QueryExecutionContextAdapter*>(ctx)->getCtxImpl();

        auto execStageHandle = UnownedExecAggStageHandle(execAggStage);
        const std::string stageName = std::string(execStageHandle.getName());

        *metrics = execCtx.getMetrics(stageName, execStageHandle)->get();
    });
}

}  // namespace mongo::extension::host_connector
