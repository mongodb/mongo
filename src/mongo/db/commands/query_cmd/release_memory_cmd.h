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

#include "mongo/db/commands.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/release_memory_gen.h"

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
