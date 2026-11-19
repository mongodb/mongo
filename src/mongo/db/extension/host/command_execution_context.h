// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host_connector/adapter/command_execution_context_adapter.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

/**
 * CommandExecutionContext is the host implementation of the command execution context. It wraps
 * the running command's OperationContext and delegates interrupt and deadline queries to it.
 *
 * It does not opt in to stepdown interrupts, matching the built-in search index management
 * commands: killOp, maxTimeMS expiry, and shutdown end the operation, but a stepdown does not.
 *
 * This class is intended for use by the extension host connector framework and should not be
 * instantiated directly by extension code.
 */
class CommandExecutionContext final : public host_connector::CommandExecutionContextBase {
public:
    explicit CommandExecutionContext(OperationContext* opCtx) : _opCtx(opCtx) {}

    Status checkForInterrupt() const override {
        return _opCtx->checkForInterruptNoAssert();
    }

    int64_t getDeadlineTimestampMs() const override {
        return _opCtx->getDeadline().asInt64();
    }

private:
    OperationContext* _opCtx;
};

}  // namespace mongo::extension::host
