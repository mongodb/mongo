// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authz_session_external_state_shard.h"

#include "mongo/base/shim.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

namespace mongo {

AuthzSessionExternalStateShard::AuthzSessionExternalStateShard(Client* client)
    : AuthzSessionExternalStateServerCommon(client) {}
AuthzSessionExternalStateShard::~AuthzSessionExternalStateShard() {}

void AuthzSessionExternalStateShard::startRequest(OperationContext* opCtx) {
    // No locks should be held as this happens before any database accesses occur
    dassert(!shard_role_details::getLocker(opCtx)->isLocked());

    _checkShouldAllowLocalhost(opCtx);
}

bool AuthzSessionExternalStateShard::shouldIgnoreAuthChecks() const {
    if (AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks()) {
        return true;
    }

    if (!haveClient()) {
        return false;
    }

    // TODO(spencer): get "isInDirectClient" from OperationContext
    return cc().isInDirectClient();
}

bool AuthzSessionExternalStateShard::serverIsArbiter() const {
    // Arbiters have access to extra privileges under localhost. See SERVER-5479.
    return (
        repl::ReplicationCoordinator::get(getGlobalServiceContext())->getSettings().isReplSet() &&
        repl::ReplicationCoordinator::get(getGlobalServiceContext())->getMemberState().arbiter());
}

}  // namespace mongo
