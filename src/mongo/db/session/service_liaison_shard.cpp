/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/session/service_liaison_shard.h"

#include "mongo/db/client.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/service_liaison_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace service_liaison_shard_callbacks {

LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) {
    LogicalSessionIdSet shardOpenCursorsSessions;

    CursorManager::get(opCtx)->appendActiveSessions(&shardOpenCursorsSessions);

    auto embeddedRouterOpenCursorsSessions = [&]() {
        if (auto service = opCtx->getServiceContext()->getService(ClusterRole::RouterServer);
            !service) {
            return LogicalSessionIdSet{};
        }

        return service_liaison_router_callbacks::getOpenCursorSessions(opCtx);
    }();

    shardOpenCursorsSessions.merge(std::move(embeddedRouterOpenCursorsSessions));
    return shardOpenCursorsSessions;
}

int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                    const SessionKiller::Matcher& matcher) {
    auto shardKilledCursors =
        CursorManager::get(opCtx)->killCursorsWithMatchingSessions(opCtx, matcher);

    auto embeddedRouterKilledCursors = [&]() {
        if (auto service = opCtx->getServiceContext()->getService(ClusterRole::RouterServer);
            !service) {
            return 0;
        }

        return service_liaison_router_callbacks::killCursorsWithMatchingSessions(opCtx, matcher);
    }();

    // Ignore errors when trying to kill cursors with matching sessions. This flow is only run by
    // the LogicalSessionCache, that eventually will call it again.
    if (!shardKilledCursors.first.isOK()) {
        LOGV2_WARNING(8391500,
                      "Error while trying to kill cursors",
                      "error"_attr = redact(shardKilledCursors.first));
    }

    return shardKilledCursors.second + embeddedRouterKilledCursors;
}

}  // namespace service_liaison_shard_callbacks

}  // namespace mongo
