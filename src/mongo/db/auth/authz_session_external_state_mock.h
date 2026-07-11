// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Mock of the AuthzSessionExternalState class used only for testing.
 */
class AuthzSessionExternalStateMock : public AuthzSessionExternalState {
    AuthzSessionExternalStateMock(const AuthzSessionExternalStateMock&) = delete;
    AuthzSessionExternalStateMock& operator=(const AuthzSessionExternalStateMock&) = delete;

public:
    AuthzSessionExternalStateMock(Client* client)
        : AuthzSessionExternalState(client),
          _ignoreAuthChecksReturnValue(false),
          _allowLocalhostReturnValue(false),
          _serverIsArbiterReturnValue(false) {}

    bool shouldIgnoreAuthChecks() const override {
        return _ignoreAuthChecksReturnValue;
    }

    bool shouldAllowLocalhost() const override {
        return _allowLocalhostReturnValue;
    }

    bool serverIsArbiter() const override {
        return _serverIsArbiterReturnValue;
    }

    void setReturnValueForShouldIgnoreAuthChecks(bool returnValue) {
        _ignoreAuthChecksReturnValue = returnValue;
    }

    void setReturnValueForShouldAllowLocalhost(bool returnValue) {
        _allowLocalhostReturnValue = returnValue;
    }

    void startRequest(OperationContext* opCtx) override {}

private:
    bool _ignoreAuthChecksReturnValue;
    bool _allowLocalhostReturnValue;
    bool _serverIsArbiterReturnValue;
};

}  // namespace mongo
