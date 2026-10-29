// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/authz_session_external_state_server_common.h"

#include "mongo/db/auth/enable_localhost_auth_bypass_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

namespace {
std::once_flag checkShouldAllowLocalhostOnceFlag;
}  // namespace

// NOTE: we default _allowLocalhost to true under the assumption that _checkShouldAllowLocalhost
// will always be called before any calls to shouldAllowLocalhost.  If this is not the case,
// it could cause a security hole.
AuthzSessionExternalStateServerCommon::AuthzSessionExternalStateServerCommon(Client* client)
    : AuthzSessionExternalState(client), _allowLocalhost(enableLocalhostAuthBypass) {}
AuthzSessionExternalStateServerCommon::~AuthzSessionExternalStateServerCommon() {}

void AuthzSessionExternalStateServerCommon::_checkShouldAllowLocalhost(OperationContext* opCtx) {
    if (!AuthorizationManager::get(opCtx->getService())->isAuthEnabled())
        return;
    // If we know that an admin user exists, don't re-check.
    if (!_allowLocalhost)
        return;
    // Don't bother checking if we're not on a localhost connection
    if (!Client::getCurrent()->getIsLocalHostConnection()) {
        _allowLocalhost = false;
        return;
    }

    _allowLocalhost =
        !AuthorizationManager::get(opCtx->getService())->hasAnyPrivilegeDocuments(opCtx);
    if (_allowLocalhost) {
        std::call_once(checkShouldAllowLocalhostOnceFlag, []() {
            LOGV2(20248,
                  "note: no users configured in admin.system.users, allowing localhost "
                  "access");
        });
    }
}

bool AuthzSessionExternalStateServerCommon::serverIsArbiter() const {
    return false;
}

bool AuthzSessionExternalStateServerCommon::shouldAllowLocalhost() const {
    if (!haveClient()) {
        return false;
    }
    Client* client = Client::getCurrent();
    return _allowLocalhost && client->getIsLocalHostConnection();
}

bool AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks() const {
    return !AuthorizationManager::get(_client->getService())->isAuthEnabled();
}

}  // namespace mongo
