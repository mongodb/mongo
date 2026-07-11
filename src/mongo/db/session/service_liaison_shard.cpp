// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/session/service_liaison_shard.h"

#include "mongo/db/client.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/service_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace service_liaison_shard_callbacks {

LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) {
    LogicalSessionIdSet shardOpenCursorsSessions;
    CursorManager::get(opCtx)->appendActiveSessions(&shardOpenCursorsSessions);
    return shardOpenCursorsSessions;
}

int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                    const SessionKiller::Matcher& matcher) {
    auto shardKilledCursors =
        CursorManager::get(opCtx)->killCursorsWithMatchingSessions(opCtx, matcher);

    // Ignore errors when trying to kill cursors with matching sessions. This flow is only run by
    // the LogicalSessionCache, that eventually will call it again.
    if (!shardKilledCursors.first.isOK()) {
        LOGV2_WARNING(8391500,
                      "Error while trying to kill cursors",
                      "error"_attr = redact(shardKilledCursors.first));
    }

    return shardKilledCursors.second;
}

}  // namespace service_liaison_shard_callbacks

}  // namespace mongo
