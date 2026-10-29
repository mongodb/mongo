// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/replication_auth.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/internal_auth.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

// Gets the singleton AuthorizationManager object for this server process
AuthorizationManager* getGlobalAuthorizationManager() {
    auto shardService = getGlobalServiceContext()->getService();
    // We can assert here that a shard Service exists since this
    // should only be called in a replication context.
    invariant(shardService != nullptr);
    AuthorizationManager* globalAuthManager = AuthorizationManager::get(shardService);
    fassert(16842, globalAuthManager != nullptr);
    return globalAuthManager;
}

}  // namespace

Status replAuthenticate(DBClientBase* conn) try {
    if (auth::isInternalAuthSet()) {
        conn->authenticateInternalUser();
    } else if (getGlobalAuthorizationManager()->isAuthEnabled())
        return {ErrorCodes::AuthenticationFailed,
                "Authentication is enabled but no internal authentication data is available."};
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace repl
}  // namespace mongo
