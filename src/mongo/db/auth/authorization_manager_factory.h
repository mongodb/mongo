// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Factory class for generating the correct authorization manager for the
 * process. createRouter creates an authorization manager that connects to
 * config servers to get authorization information, and createShard creates
 * an authorization manager that may search locally for authorization
 * information unless the user is registered to $external.
 */

class AuthorizationManagerFactory {

public:
    virtual ~AuthorizationManagerFactory() = default;

    virtual std::unique_ptr<AuthorizationManager> createRouter(Service* service) = 0;
    virtual std::unique_ptr<AuthorizationManager> createShard(Service* service) = 0;

    virtual std::unique_ptr<auth::AuthorizationBackendInterface> createBackendInterface(
        Service* service) = 0;

    virtual Status initialize(OperationContext* opCtx) = 0;
};

extern std::unique_ptr<AuthorizationManagerFactory> globalAuthzManagerFactory;

}  // namespace mongo
