/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <memory>
#include <utility>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/sequence_util.h"

namespace mongo {
namespace {

const auto getAuthorizationManager =
    ServiceContext::declareDecoration<std::unique_ptr<AuthorizationManager>>();

const auto getAuthorizationSession =
    Client::declareDecoration<std::unique_ptr<AuthorizationSession>>();

struct DisabledAuthMechanisms {
    bool x509 = false;
};

const auto getDisabledAuthMechanisms = ServiceContext::declareDecoration<DisabledAuthMechanisms>();

const auto getClusterAuthMode =
    ServiceContext::declareDecoration<synchronized_value<ClusterAuthMode>>();

class AuthzClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {
        if (auto authzManager = AuthorizationManager::get(client->getServiceContext())) {
            AuthorizationSession::set(client, authzManager->makeAuthorizationSession());
        }
    }

    void onDestroyClient(Client* client) override {
        // Logout before the client is destroyed.
        auto& authzSession = getAuthorizationSession(client);
        if (authzSession) {
            authzSession->logoutAllDatabases(client, "Client has disconnected");
        }
    }

    void onCreateOperationContext(OperationContext* opCtx) override {}
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

auto disableAuthMechanismsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "DisableAuthMechanisms", [](ServiceContext* service) {
        if (!sequenceContains(saslGlobalParams.authenticationMechanisms, kX509AuthMechanism)) {
            disableX509Auth(service);
        }
    }};

ServiceContext::ConstructorActionRegisterer authzClientObserverRegisterer{
    "AuthzClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<AuthzClientObserver>());
    }};

ServiceContext::ConstructorActionRegisterer destroyAuthorizationManagerRegisterer(
    "DestroyAuthorizationManager",
    [](ServiceContext* service) {
        // Intentionally empty, since construction happens through different code paths depending on
        // the binary
    },
    [](ServiceContext* service) { AuthorizationManager::set(service, nullptr); });

}  // namespace

AuthorizationManager* AuthorizationManager::get(ServiceContext* service) {
    return getAuthorizationManager(service).get();
}

AuthorizationManager* AuthorizationManager::get(ServiceContext& service) {
    return getAuthorizationManager(service).get();
}

void AuthorizationManager::set(ServiceContext* service,
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
    invariant(!authzSession);
    authzSession = std::move(authorizationSession);
}

ClusterAuthMode ClusterAuthMode::get(ServiceContext* svcCtx) {
    return getClusterAuthMode(svcCtx).get();
}

ClusterAuthMode ClusterAuthMode::set(ServiceContext* svcCtx, const ClusterAuthMode& mode) {
    auto sv = getClusterAuthMode(svcCtx).synchronize();
    if (!sv->canTransitionTo(mode)) {
        uasserted(5579202,
                  fmt::format("Illegal state transition for clusterAuthMode from '{}' to '{}'",
                              sv->toString(),
                              mode.toString()));
    }
    return std::exchange(*sv, mode);
}

void disableX509Auth(ServiceContext* svcCtx) {
    getDisabledAuthMechanisms(svcCtx).x509 = true;
}

bool isX509AuthDisabled(ServiceContext* svcCtx) {
    return getDisabledAuthMechanisms(svcCtx).x509;
}

}  // namespace mongo
