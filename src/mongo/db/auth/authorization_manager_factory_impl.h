// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace mongo {

class AuthorizationManagerFactoryImpl : public AuthorizationManagerFactory {
public:
    std::unique_ptr<AuthorizationManager> createRouter(Service* service) final;
    std::unique_ptr<AuthorizationManager> createShard(Service* service) final;

    std::unique_ptr<auth::AuthorizationBackendInterface> createBackendInterface(
        Service* service) override;

    // No-op.
    Status initialize(OperationContext* opCtx) override {
        return Status::OK();
    }
};

}  // namespace mongo
