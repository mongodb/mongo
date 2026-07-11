// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/release_memory_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A command for asking existing cursors, registered with a CursorManager to release memory.
 */
class ReleaseMemoryCmd final : public TypedCommand<ReleaseMemoryCmd> {
public:
    using Request = ReleaseMemoryCommandRequest;
    using Response = ReleaseMemoryCommandReply;

    ReleaseMemoryCmd() : TypedCommand("releaseMemory") {}

    const std::set<std::string>& apiVersions() const override;
    bool allowedInTransactions() const final;
    bool allowedWithSecurityToken() const final;
    bool enableDiagnosticPrintingOnFailure() const final;

    static Status releaseMemory(OperationContext* opCtx, ClientCursorPin& cursorPin);

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ReleaseMemoryCommandReply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const override;
        NamespaceString ns() const override;
        const DatabaseName& db() const override;
        void doCheckAuthorization(OperationContext* opCtx) const override;
        const GenericArguments& getGenericArguments() const override;
        Status acquireLocksAndReleaseMemory(OperationContext* opCtx, ClientCursorPin& cursorPin);
    };
    bool maintenanceOk() const override;
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override;
    std::string help() const override;
    LogicalOp getLogicalOp() const override;
    bool collectsResourceConsumptionMetrics() const final;
    bool shouldAffectCommandCounter() const final;
    bool adminOnly() const final;
};
}  // namespace mongo
