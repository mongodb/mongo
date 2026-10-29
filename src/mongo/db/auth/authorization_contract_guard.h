// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] AuthorizationContractGuard {
public:
    explicit AuthorizationContractGuard(AuthorizationSession* authSession)
        : _authSession(authSession) {
        if (_authSession) {
            _authSession->startContractTracking();
        }
    }

    ~AuthorizationContractGuard() {
        if (_authSession) {
            _authSession->endContractTracking();
        }
    }

    AuthorizationContractGuard(const AuthorizationContractGuard&) = delete;
    AuthorizationContractGuard& operator=(const AuthorizationContractGuard&) = delete;

    AuthorizationContractGuard(AuthorizationContractGuard&& other) = delete;
    AuthorizationContractGuard& operator=(AuthorizationContractGuard&& other) = delete;

private:
    AuthorizationSession* _authSession;
};

}  // namespace mongo
