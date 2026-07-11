// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/authorization_manager_factory_mock.h"

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_client_handle_router.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/auth/authorization_router_impl_for_test.h"

namespace mongo {

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryMock::createRouter(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImplForTest>(
        service, std::make_unique<AuthorizationClientHandleRouter>());

    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<AuthorizationManager> AuthorizationManagerFactoryMock::createShard(
    Service* service) {
    auto authzRouter = std::make_unique<AuthorizationRouterImplForTest>(
        service, std::make_unique<AuthorizationClientHandleShard>());

    return std::make_unique<AuthorizationManagerImpl>(service, std::move(authzRouter));
}

std::unique_ptr<auth::AuthorizationBackendInterface>
AuthorizationManagerFactoryMock::createBackendInterface(Service* service) {
    return std::make_unique<auth::AuthorizationBackendMock>();
}

namespace {

MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())(InitializerContext*) {}

MONGO_INITIALIZER(RegisterGlobalAuthzManagerFactoryMock)(InitializerContext* initializer) {
    globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
}

}  // namespace

}  // namespace mongo
