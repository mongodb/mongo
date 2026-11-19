// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/host_connector/adapter/command_execution_context_adapter.h"

#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host_connector {

MongoExtensionStatus* CommandExecutionContextAdapter::_extCheckForInterrupt(
    const MongoExtensionCommandExecutionContext* ctx, MongoExtensionStatus* opStatus) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const CommandExecutionContextAdapter*>(ctx)->getCtxImpl();
        Status interrupted = execCtx.checkForInterrupt();
        // Populate opStatus on both paths so a caller that reuses a mutable status cannot observe a
        // stale interruption for a healthy operation.
        auto statusAPI = StatusAPI(opStatus);
        statusAPI.setCode(interrupted.code());
        statusAPI.setReason(interrupted.reason());
    });
}

MongoExtensionStatus* CommandExecutionContextAdapter::_extGetDeadlineTimestampMs(
    const MongoExtensionCommandExecutionContext* ctx, int64_t* deadlineTimestampMs) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& execCtx = static_cast<const CommandExecutionContextAdapter*>(ctx)->getCtxImpl();
        *deadlineTimestampMs = execCtx.getDeadlineTimestampMs();
    });
}

}  // namespace mongo::extension::host_connector
