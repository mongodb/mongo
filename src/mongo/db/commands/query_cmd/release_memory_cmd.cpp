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

#include "mongo/db/commands/query_cmd/release_memory_cmd.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/release_memory_gen.h"
#include "mongo/db/query/client_cursor/release_memory_util.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy_release_memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery
namespace mongo {

MONGO_FAIL_POINT_DEFINE(releaseMemoryHangAfterPinCursor);

const std::set<std::string>& ReleaseMemoryCmd::apiVersions() const {
    return kNoApiVersions;
}

bool ReleaseMemoryCmd::allowedInTransactions() const {
    return true;
}

bool ReleaseMemoryCmd::allowedWithSecurityToken() const {
    return true;
}

bool ReleaseMemoryCmd::enableDiagnosticPrintingOnFailure() const {
    return true;
}

Status ReleaseMemoryCmd::releaseMemory(OperationContext* opCtx, ClientCursorPin& cursorPin) {
    try {
        PlanExecutor* exec = cursorPin->getExecutor();

        std::unique_ptr<PlanYieldPolicy> yieldPolicy = nullptr;
        if (exec->lockPolicy() == PlanExecutor::LockPolicy::kLockExternally) {
            yieldPolicy = PlanYieldPolicyReleaseMemory::make(
                opCtx, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY, boost::none, exec->nss());
        }

        exec->reattachToOperationContext(opCtx);
        ScopeGuard opCtxGuard([&]() { exec->detachFromOperationContext(); });
        exec->forceSpill(yieldPolicy.get());

        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

ReleaseMemoryCommandReply ReleaseMemoryCmd::Invocation::typedRun(OperationContext* opCtx) {
    std::vector<CursorId> released;
    std::vector<CursorId> notFound;
    std::vector<CursorId> currentlyPinned;
    std::vector<ReleaseMemoryError> errors;

    auto handleError = [&](CursorId cursorId, Status status) {
        ReleaseMemoryError error{cursorId};
        error.setStatus(status);
        errors.push_back(std::move(error));
        LOGV2_ERROR(10217801,
                    "ReleaseMemory returned unexpected status",
                    "cursorId"_attr = cursorId,
                    "status"_attr = status.toString());
    };

    for (CursorId cursorId : request().getCommandParameter()) {
        if (auto status = opCtx->checkForInterruptNoAssert(); !status.isOK()) {
            break;
        }

        auto pinCheck = [&](const ClientCursor& cc) {
            uassertStatusOK(auth::checkAuthForReleaseMemory(
                AuthorizationSession::get(opCtx->getClient()), cc.nss()));
        };

        auto cursorPin = CursorManager::get(opCtx)->pinCursor(
            opCtx, cursorId, definition()->getName(), pinCheck);
        if (cursorPin.isOK()) {
            ScopeGuard killCursorGuard([&cursorPin] {
                // Something went wrong. Destroy the cursor.
                cursorPin.getValue().deleteUnderlying();
            });

            if (MONGO_unlikely(releaseMemoryHangAfterPinCursor.shouldFail())) {
                LOGV2(9745500,
                      "releaseMemoryHangAfterPinCursor fail point enabled. Blocking until "
                      "failpoint is disabled");
                releaseMemoryHangAfterPinCursor.pauseWhileSet(opCtx);
            }

            Status response = Status::OK();
            const NamespaceString& nss = cursorPin.getValue().getCursor()->nss();
            failReleaseMemoryAfterCursorCheckout.executeIf(
                [&](const BSONObj& data) {
                    auto errorCode =
                        (data["errorCode"] ? ErrorCodes::Error{data["errorCode"].safeNumberInt()}
                                           : ErrorCodes::InternalError);
                    response = Status{errorCode,
                                      "Hit the 'failReleaseMemoryAfterCursorCheckout' failpoint"};
                },
                [&opCtx, nss](const BSONObj& data) {
                    auto dataForFailCommand = data.addField(
                        BSON("failCommands" << BSON_ARRAY("releaseMemory")).firstElement());
                    auto* command = CommandHelpers::findCommand(opCtx, "releaseMemory");
                    return CommandHelpers::shouldActivateFailCommandFailPoint(
                        dataForFailCommand, nss, command, opCtx->getClient());
                });

            if (response.isOK()) {
                response = releaseMemory(opCtx, cursorPin.getValue());
            }

            if (response.isOK()) {
                released.push_back(cursorId);
                killCursorGuard.dismiss();
            } else {
                handleError(cursorId, response);
                // Do not destroy the cursor if the error is
                // QueryExceededMemoryLimitNoDiskUseAllowed since at this point spilling has
                // not yet started.
                if (response.code() == ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed) {
                    killCursorGuard.dismiss();
                }
            }
        } else if (cursorPin.getStatus().code() == ErrorCodes::CursorNotFound) {
            notFound.push_back(cursorId);
        } else if (cursorPin.getStatus().code() == ErrorCodes::CursorInUse) {
            currentlyPinned.push_back(cursorId);
        } else {
            handleError(cursorId, cursorPin.getStatus());
        }
    }

    return ReleaseMemoryCommandReply{
        std::move(released), std::move(notFound), std::move(currentlyPinned), std::move(errors)};
}

bool ReleaseMemoryCmd::Invocation::supportsWriteConcern() const {
    return false;
}

NamespaceString ReleaseMemoryCmd::Invocation::ns() const {
    return NamespaceString::makeCommandNamespace(db());
}

const DatabaseName& ReleaseMemoryCmd::Invocation::db() const {
    return request().getDbName();
}

void ReleaseMemoryCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    for (CursorId cursorId : request().getCommandParameter()) {
        auto status = CursorManager::get(opCtx)->checkAuthForReleaseMemory(opCtx, cursorId);
        if (status.code() == ErrorCodes::CursorNotFound) {
            continue;
        }
        uassertStatusOK(status);
    }
}

const GenericArguments& ReleaseMemoryCmd::Invocation::getGenericArguments() const {
    return request().getGenericArguments();
}

bool ReleaseMemoryCmd::maintenanceOk() const {
    return false;
}

ReleaseMemoryCmd::AllowedOnSecondary ReleaseMemoryCmd::secondaryAllowed(ServiceContext*) const {
    return AllowedOnSecondary::kAlways;
}

std::string ReleaseMemoryCmd::help() const {
    return "ask given cursors to release memory if possible";
}

LogicalOp ReleaseMemoryCmd::getLogicalOp() const {
    return LogicalOp::opCommand;
}

bool ReleaseMemoryCmd::collectsResourceConsumptionMetrics() const {
    return true;
}

bool ReleaseMemoryCmd::shouldAffectCommandCounter() const {
    return true;
}

bool ReleaseMemoryCmd::adminOnly() const {
    return false;
}

MONGO_REGISTER_COMMAND(ReleaseMemoryCmd).forShard();
}  // namespace mongo
