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

#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/acquire_locks.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/release_memory_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery
namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(releaseMemoryHangAfterPinCursor);

/**
 * A command for asking existing cursors, registered with a CursorManager to release memory.
 */
class ReleaseMemoryCmd final : public TypedCommand<ReleaseMemoryCmd> {
public:
    using Request = ReleaseMemoryCommandRequest;
    using Response = ReleaseMemoryCommandReply;

    ReleaseMemoryCmd() : TypedCommand("releaseMemory") {}

    const std::set<std::string>& apiVersions() const override {
        return kNoApiVersions;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ReleaseMemoryCommandReply typedRun(OperationContext* opCtx) {
            std::vector<CursorId> released;
            std::vector<CursorId> notFound;
            std::vector<CursorId> currentlyPinned;

            for (CursorId cursorId : request().getCommandParameter()) {
                auto cursorPin = CursorManager::get(opCtx)->pinCursor(opCtx, cursorId);
                if (cursorPin.isOK()) {
                    if (MONGO_unlikely(releaseMemoryHangAfterPinCursor.shouldFail())) {
                        LOGV2(9745500,
                              "releaseMemoryHangAfterPinCursor fail point enabled. Blocking until "
                              "fail "
                              "point is disabled");
                        releaseMemoryHangAfterPinCursor.pauseWhileSet(opCtx);
                    }

                    acquireLocksAndReleaseMemory(opCtx, cursorPin.getValue());
                    released.push_back(cursorId);
                } else if (cursorPin.getStatus().code() == ErrorCodes::CursorNotFound) {
                    notFound.push_back(cursorId);
                } else if (cursorPin.getStatus().code() == ErrorCodes::CursorInUse) {
                    currentlyPinned.push_back(cursorId);
                } else {
                    uassertStatusOK(cursorPin.getStatus());
                }
            }

            return ReleaseMemoryCommandReply{
                std::move(released), std::move(notFound), std::move(currentlyPinned)};
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::makeCommandNamespace(db());
        }

        const DatabaseName& db() const override {
            return request().getDbName();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            for (CursorId cursorId : request().getCommandParameter()) {
                auto status = CursorManager::get(opCtx)->checkAuthForReleaseMemory(opCtx, cursorId);
                if (status.code() == ErrorCodes::CursorNotFound) {
                    continue;
                }
                uassertStatusOK(status);
            }
        }

        const GenericArguments& getGenericArguments() const override {
            return request().getGenericArguments();
        }

        void acquireLocksAndReleaseMemory(OperationContext* opCtx, ClientCursorPin& cursorPin) {
            applyConcernsAndReadPreference(opCtx, *cursorPin.getCursor());
            CursorLocks locks{opCtx, cursorPin.getCursor()->nss(), cursorPin};

            if (!cursorPin->isAwaitData()) {
                opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.
            }

            PlanExecutor* exec = cursorPin->getExecutor();
            exec->reattachToOperationContext(opCtx);
            ScopeGuard opCtxGuard([&]() { exec->detachFromOperationContext(); });
            exec->forceSpill();
        }
    };

    bool maintenanceOk() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "ask given cursors to release memory if possible";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opCommand;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return true;
    }

    bool adminOnly() const final {
        return false;
    }
};
MONGO_REGISTER_COMMAND(ReleaseMemoryCmd).forShard();

}  // namespace
}  // namespace mongo
