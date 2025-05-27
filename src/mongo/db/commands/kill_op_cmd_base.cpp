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
#include "mongo/db/commands/kill_op_cmd_base.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_killer.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <limits>
#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

void KillOpCmdBase::reportSuccessfulCompletion(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const BSONObj& cmdObj) {

    logv2::DynamicAttributes attr;

    auto client = opCtx->getClient();
    if (client) {
        if (AuthorizationManager::get(client->getService())->isAuthEnabled()) {
            if (auto user = AuthorizationSession::get(client)->getAuthenticatedUserName()) {
                attr.add("user", BSON_ARRAY(user->toBSON()));
            } else {
                attr.add("user", BSONArray());
            }
        }

        if (client->session()) {
            attr.add("remote", client->session()->remote());
        }

        if (auto metadata = ClientMetadata::get(client)) {
            attr.add("metadata", metadata->getDocument());
        }
    }

    attr.add("db", dbName);
    attr.add("command", cmdObj);

    LOGV2(558700, "Successful killOp", attr);
}


Status KillOpCmdBase::checkAuthForOperation(OperationContext* workerOpCtx,
                                            const DatabaseName&,
                                            const BSONObj& cmdObj) const {
    auto* worker = workerOpCtx->getClient();
    auto opKiller = OperationKiller(worker);

    if (opKiller.isGenerallyAuthorizedToKill()) {
        return Status::OK();
    }

    if (isKillingLocalOp(cmdObj.getField("op"))) {
        // Look up the OperationContext and see if we have permission to kill it. This is done once
        // here and again in the command body. The check here in the checkAuthForOperation function
        // is necessary because if the check fails, it will be picked up by the auditing system.
        long long opId = parseOpId(cmdObj);
        auto target = worker->getServiceContext()->getLockedClient(opId);

        if (OperationKiller(worker).isAuthorizedToKill(target)) {
            // We were authorized to interact with the target Client
            return Status::OK();
        }
    }

    return Status(ErrorCodes::Unauthorized, "Unauthorized");
}

void KillOpCmdBase::killLocalOperation(OperationContext* opCtx, OperationId opToKill) {
    OperationKiller(opCtx->getClient()).killOperation(opToKill);
}

bool KillOpCmdBase::isKillingLocalOp(const BSONElement& opElem) {
    return opElem.isNumber();
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
