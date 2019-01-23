
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/kill_op_cmd_base.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

Status KillOpCmdBase::checkAuthForCommand(Client* client,
                                          const std::string& dbname,
                                          const BSONObj& cmdObj) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);

    if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::killop)) {
        // If we have administrative permission to run killop, we don't need to traverse the
        // Client list to figure out if we own the operation which will be terminated.
        return Status::OK();
    }

    if (authzSession->isAuthenticated() && isKillingLocalOp(cmdObj.getField("op"))) {
        // Look up the OperationContext and see if we have permission to kill it. This is done once
        // here and again in the command body. The check here in the checkAuthForCommand() function
        // is necessary because if the check fails, it will be picked up by the auditing system.
        long long opId = parseOpId(cmdObj);
        auto lkAndOp = KillOpCmdBase::findOpForKilling(client, opId);
        if (lkAndOp) {
            // We were able to find the Operation, and we were authorized to interact with it.
            return Status::OK();
        }
    }
    return Status(ErrorCodes::Unauthorized, "Unauthorized");
}


bool KillOpCmdBase::isKillingLocalOp(const BSONElement& opElem) {
    return opElem.isNumber();
}

boost::optional<std::tuple<stdx::unique_lock<Client>, OperationContext*>>
KillOpCmdBase::findOperationContext(ServiceContext* serviceContext, unsigned int opId) {
    for (ServiceContext::LockedClientsCursor cursor(serviceContext);
         Client* opClient = cursor.next();) {
        stdx::unique_lock<Client> lk(*opClient);

        OperationContext* opCtx = opClient->getOperationContext();
        if (opCtx && opCtx->getOpID() == opId) {
            return {std::make_tuple(std::move(lk), opCtx)};
        }
    }

    return boost::none;
}

boost::optional<std::tuple<stdx::unique_lock<Client>, OperationContext*>>
KillOpCmdBase::findOpForKilling(Client* client, unsigned int opId) {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);

    auto lockAndOpCtx = findOperationContext(client->getServiceContext(), opId);
    if (lockAndOpCtx) {
        OperationContext* opToKill = std::get<1>(*lockAndOpCtx);
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::killop) ||
            authzSession->isCoauthorizedWithClient(opToKill->getClient())) {
            return lockAndOpCtx;
        }
    }

    return boost::none;
}

void KillOpCmdBase::killLocalOperation(OperationContext* opCtx, unsigned int opToKill) {
    stdx::unique_lock<Client> lk;
    OperationContext* opCtxToKill;
    auto lockAndOpCtx = findOpForKilling(opCtx->getClient(), opToKill);
    if (!lockAndOpCtx) {
        // killOp always reports success past the auth check.
        return;
    }

    std::tie(lk, opCtxToKill) = std::move(*lockAndOpCtx);

    invariant(lk);
    opCtx->getServiceContext()->killOperation(lk, opCtxToKill);
}

unsigned int KillOpCmdBase::parseOpId(const BSONObj& cmdObj) {
    long long op;
    uassertStatusOK(bsonExtractIntegerField(cmdObj, "op", &op));

    uassert(26823,
            str::stream() << "invalid op : " << op << ". Op ID cannot be represented with 32 bits",
            (op >= std::numeric_limits<int>::min()) && (op <= std::numeric_limits<int>::max()));

    return static_cast<unsigned int>(op);
}

}  // namespace mongo
