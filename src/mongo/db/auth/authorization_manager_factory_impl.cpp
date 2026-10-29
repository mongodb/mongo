// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/authorization_manager_factory_impl.h"

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_client_handle_router.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"

namespace mongo {

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryImpl::createRouter(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImpl>(
        service, std::make_unique<AuthorizationClientHandleRouter>());

    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryImpl::createShard(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImpl>(
        service, std::make_unique<AuthorizationClientHandleShard>());
    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<auth::AuthorizationBackendInterface>
AuthorizationManagerFactoryImpl::createBackendInterface(Service* service) {
    invariant(service->role().has(ClusterRole::ShardServer) ||
              service->role().has(ClusterRole::ConfigServer));
    return std::make_unique<auth::AuthorizationBackendLocal>();
}

namespace {

MONGO_INITIALIZER(RegisterGlobalAuthzManagerFactory)(InitializerContext* initializer) {
    globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryImpl>();
}

}  // namespace

}  // namespace mongo
