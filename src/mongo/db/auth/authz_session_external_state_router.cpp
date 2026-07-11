// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authz_session_external_state_router.h"

#include "mongo/base/shim.h"
#include "mongo/db/auth/authz_session_external_state.h"

#include <memory>
#include <string>

namespace mongo {

AuthzSessionExternalStateRouter::AuthzSessionExternalStateRouter(Client* client)
    : AuthzSessionExternalStateServerCommon(client) {}
AuthzSessionExternalStateRouter::~AuthzSessionExternalStateRouter() {}

void AuthzSessionExternalStateRouter::startRequest(OperationContext* opCtx) {
    _checkShouldAllowLocalhost(opCtx);
}

}  // namespace mongo
