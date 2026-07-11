// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/session/service_liaison_router.h"

#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace service_liaison_router_callbacks {

LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx) {
    LogicalSessionIdSet openCursorSessions;

    // Append any in-use session ids from the global cluster cursor managers.
    auto cursorManager = Grid::get(opCtx->getServiceContext())->getCursorManager();
    cursorManager->appendActiveSessions(&openCursorSessions);

    return openCursorSessions;
}

int killCursorsWithMatchingSessions(OperationContext* opCtx,
                                    const SessionKiller::Matcher& matcher) {
    auto cursorManager = Grid::get(opCtx->getServiceContext())->getCursorManager();

    auto killedCursorsResponse = cursorManager->killCursorsWithMatchingSessions(opCtx, matcher);

    // Ignore errors when trying to kill cursors with matching sessions. This flow is only run by
    // the LogicalSessionCache, that eventually will call it again.
    if (!killedCursorsResponse.first.isOK()) {
        LOGV2_WARNING(8391501,
                      "Error while trying to kill cursors",
                      "error"_attr = redact(killedCursorsResponse.first));
    }

    return killedCursorsResponse.second;
}

}  // namespace service_liaison_router_callbacks

}  // namespace mongo
