
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

#include "mongo/db/pipeline/mongo_process_common.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

std::vector<BSONObj> MongoProcessCommon::getCurrentOps(OperationContext* opCtx,
                                                       CurrentOpConnectionsMode connMode,
                                                       CurrentOpSessionsMode sessionMode,
                                                       CurrentOpUserMode userMode,
                                                       CurrentOpTruncateMode truncateMode) const {
    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    std::vector<BSONObj> ops;

    for (ServiceContext::LockedClientsCursor cursor(opCtx->getClient()->getServiceContext());
         Client* client = cursor.next();) {
        invariant(client);

        stdx::lock_guard<Client> lk(*client);

        // If auth is disabled, ignore the allUsers parameter.
        if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
            userMode == CurrentOpUserMode::kExcludeOthers &&
            !ctxAuth->isCoauthorizedWithClient(client, lk)) {
            continue;
        }

        // Ignore inactive connections unless 'idleConnections' is true.
        if (!client->getOperationContext() && connMode == CurrentOpConnectionsMode::kExcludeIdle) {
            continue;
        }

        // Delegate to the mongoD- or mongoS-specific implementation of _reportCurrentOpForClient.
        ops.emplace_back(_reportCurrentOpForClient(opCtx, client, truncateMode));
    }

    // If we need to report on idle Sessions, defer to the mongoD or mongoS implementations.
    if (sessionMode == CurrentOpSessionsMode::kIncludeIdle) {
        _reportCurrentOpsForIdleSessions(opCtx, userMode, &ops);
    }

    return ops;
}

}  // namespace mongo
