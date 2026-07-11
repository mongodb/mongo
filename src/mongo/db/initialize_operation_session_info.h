// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Parses the session information from a request and stores the sessionId and txnNumber
 * on the current operation context. Must only be called once per operation and should be done right
 * in the beginning. Note that the session info will be stored in the operation context and returned
 * only if the current request supports it. For example, if attachToOpCtx is false or this is called
 * within the context of DBDirectClient.
 *
 * Throws if the sessionId/txnNumber combination is not properly formatted.
 *
 * requiresAuth specifies if the command we're initializing operationSessionInfo for requires
 * authorization or not.  This can be determined by invoking ->requiresAuth() on the parsed command.
 * If it does not require authorization, return boost::none.
 *
 * isReplSetMemberOrMongos needs to be true if the command contains a transaction number, otherwise
 * this function will throw.
 */
[[MONGO_MOD_PUBLIC]] OperationSessionInfoFromClient initializeOperationSessionInfo(
    OperationContext* opCtx,
    const boost::optional<TenantId>& validatedTenantId,
    const OperationSessionInfoFromClientBase& osi,
    bool requiresAuth,
    bool attachToOpCtx,
    bool isReplSetMemberOrMongos);

}  // namespace mongo
