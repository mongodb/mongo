// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

const auto getAuthorizationManager =
    Service::declareDecoration<std::unique_ptr<AuthorizationManager>>();

struct DisabledAuthMechanisms {
    bool x509 = false;
};

const auto getAuthorizationSession =
    Client::declareDecoration<std::unique_ptr<AuthorizationSession>>();

const auto getDisabledAuthMechanisms = Service::declareDecoration<DisabledAuthMechanisms>();

const auto getClusterAuthMode = ServiceContext::declareDecoration<Atomic<ClusterAuthMode>>();

const auto getAuthorizationBackendInterface =
    Service::declareDecoration<std::unique_ptr<auth::AuthorizationBackendInterface>>();

class AuthzClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {
        if (auto authzManager = AuthorizationManager::get(client->getService())) {
            AuthorizationSession::set(client, authzManager->makeAuthorizationSession(client));
        }
    }

    void onDestroyClient(Client* client) override {
        // Logout before the client is destroyed.
        auto& authzSession = getAuthorizationSession(client);
        if (authzSession) {
            authzSession->logoutAllDatabases("Client has disconnected");
        }
    }

    void onCreateOperationContext(OperationContext* opCtx) override {}
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};


ServiceContext::ConstructorActionRegisterer authzClientObserverRegisterer{
    "AuthzClientObserver", [](ServiceContext* svCtx) {
        svCtx->registerClientObserver(std::make_unique<AuthzClientObserver>());
    }};

}  // namespace

AuthorizationManager* AuthorizationManager::get(Service* service) {
    return getAuthorizationManager(service).get();
}

AuthorizationManager* AuthorizationManager::get(Service& service) {
    return getAuthorizationManager(service).get();
}

auth::AuthorizationBackendInterface* auth::AuthorizationBackendInterface::get(Service* service) {
    return getAuthorizationBackendInterface(service).get();
}

void auth::AuthorizationBackendInterface::set(
    Service* service, std::unique_ptr<AuthorizationBackendInterface> backendInterface) {
    getAuthorizationBackendInterface(service) = std::move(backendInterface);
}

void AuthorizationManager::set(Service* service,
                               std::unique_ptr<AuthorizationManager> authzManager) {
    getAuthorizationManager(service) = std::move(authzManager);
}

AuthorizationSession* AuthorizationSession::get(Client* client) {
    return get(*client);
}

AuthorizationSession* AuthorizationSession::get(Client& client) {
    AuthorizationSession* retval = getAuthorizationSession(client).get();
    massert(16481, "No AuthorizationManager has been set up for this connection", retval);
    return retval;
}

bool AuthorizationSession::exists(Client* client) {
    return getAuthorizationSession(client).get();
}

void AuthorizationSession::set(Client* client,
                               std::unique_ptr<AuthorizationSession> authorizationSession) {
    auto& authzSession = getAuthorizationSession(client);
    invariant(authorizationSession);
    authzSession = std::move(authorizationSession);
}

ClusterAuthMode ClusterAuthMode::get(ServiceContext* svcCtx) {
    return getClusterAuthMode(svcCtx).loadRelaxed();
}

ClusterAuthMode ClusterAuthMode::set(ServiceContext* svcCtx, const ClusterAuthMode& newMode) {
    auto& authMode = getClusterAuthMode(svcCtx);
    auto current = authMode.load();
    do {
        uassert(5579202,
                fmt::format("Illegal state transition for clusterAuthMode from '{}' to '{}'",
                            current.toString(),
                            newMode.toString()),
                current.canTransitionTo(newMode));
    } while (!authMode.compareAndSwap(&current, newMode));
    return current;
}

}  // namespace mongo
