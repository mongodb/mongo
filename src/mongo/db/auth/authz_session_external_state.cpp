// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authz_session_external_state.h"

#include "mongo/base/shim.h"
#include "mongo/db/auth/authorization_manager.h"

#include <string>

namespace mongo {

AuthzSessionExternalState::AuthzSessionExternalState(Client* client) : _client(client) {}
AuthzSessionExternalState::~AuthzSessionExternalState() {}

}  // namespace mongo
