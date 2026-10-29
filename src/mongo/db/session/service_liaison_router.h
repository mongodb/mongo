// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This encapsulates the callbacks to implement the methods to return the cursors for the opened
 * sessions or kill the cursors that matches the given session for a logical session cache acting as
 * a router.
 */
namespace service_liaison_router_callbacks {

LogicalSessionIdSet getOpenCursorSessions(OperationContext* opCtx);

int killCursorsWithMatchingSessions(OperationContext* opCtx, const SessionKiller::Matcher& matcher);

}  // namespace service_liaison_router_callbacks

}  // namespace mongo
