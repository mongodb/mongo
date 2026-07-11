// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Factory class for generating the correct authorization manager for the
 * process. The create function returns the correct authorization manager
 * based on the arguments provided.
 */

class [[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] AuthorizationManagerFactoryMock
    : public AuthorizationManagerFactory {

public:
    std::unique_ptr<AuthorizationManager> createRouter(Service* service) override;
    std::unique_ptr<AuthorizationManager> createShard(Service* service) override;

    std::unique_ptr<auth::AuthorizationBackendInterface> createBackendInterface(
        Service* service) override;

    Status initialize(OperationContext* opCtx) override {
        return Status::OK();
    }
};


}  // namespace mongo
