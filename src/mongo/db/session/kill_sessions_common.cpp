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


#include "mongo/db/session/kill_sessions_common.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/session.h"
#include "mongo/util/net/hostandport.h"

#include <mutex>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

SessionKiller::Result killSessionsLocalKillOps(OperationContext* opCtx,
                                               const SessionKiller::Matcher& matcher) {
    for (ServiceContext::LockedClientsCursor cursor(opCtx->getClient()->getServiceContext());
         Client* client = cursor.next();) {
        invariant(client);
        ClientLock lk(client);

        OperationContext* opCtxToKill = client->getOperationContext();
        if (opCtxToKill) {
            const auto& lsid = opCtxToKill->getLogicalSessionId();
            const auto exempt = opCtxToKill->isKillOpsExempt();

            if (lsid && !exempt) {
                if (const KillAllSessionsByPattern* pattern = matcher.match(*lsid)) {
                    ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);

                    LOGV2(20706,
                          "Killing op as part of killing session",
                          "opId"_attr = opCtxToKill->getOpID(),
                          "lsid"_attr = lsid->toBSON());

                    opCtx->getServiceContext()->killOperation(lk, opCtxToKill);
                }
            }
        }
    }

    return {std::vector<HostAndPort>{}};
}

Status killSessionsCmdHelper(OperationContext* opCtx, const KillAllSessionsByPatternSet& patterns) {
    auto killResult = SessionKiller::get(opCtx)->kill(opCtx, patterns);
    if (!killResult->isOK()) {
        return killResult->getStatus();
    }

    if (!killResult->getValue().empty()) {
        LOGV2_ERROR(8963000,
                    "Failed to kill sessions on some hosts",
                    "failedHosts"_attr = killResult->getValue());
        return Status(ErrorCodes::HostUnreachable, "Failed to kill sessions on some hosts");
    }

    return Status::OK();
}

void killSessionsReport(OperationContext* opCtx, const BSONObj& cmdObj) {

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
            attr.add("session", client->session()->toBSON());
        }

        if (auto metadata = ClientMetadata::get(client)) {
            attr.add("metadata", metadata->getDocument());
        }
    }

    attr.add("command", cmdObj);

    LOGV2(558701, "Success: kill session", attr);
}

}  // namespace mongo
