// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/internal_auth.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_client_handle.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

ServiceContext::ConstructorActionRegisterer setClusterAuthMode(
    "SetClusterAuthMode", [](ServiceContext* serviceContext) {
        // Officially set the ClusterAuthMode for this ServiceContext
        if (!ClusterAuthMode::get(serviceContext)
                 .equals(serverGlobalParams.startupClusterAuthMode)) {
            ClusterAuthMode::set(serviceContext, serverGlobalParams.startupClusterAuthMode);
        }
    });

Service::ConstructorActionRegisterer createAuthorizationManager(
    "CreateAuthorizationManager",
    {"OIDGeneration", "EndStartupOptionStorage"},
    [](Service* service) {
        const auto clusterAuthMode = serverGlobalParams.startupClusterAuthMode;
        const auto authIsEnabled =
            serverGlobalParams.authState == ServerGlobalParams::AuthState::kEnabled;

        std::unique_ptr<AuthorizationManager> authzManager;

        if (service->role().hasExclusively(ClusterRole::RouterServer)) {
            authzManager = globalAuthzManagerFactory->createRouter(service);
        } else {
            authzManager = globalAuthzManagerFactory->createShard(service);
            auth::AuthorizationBackendInterface::set(
                service, globalAuthzManagerFactory->createBackendInterface(service));
        }

        authzManager->setAuthEnabled(authIsEnabled);
        authzManager->setShouldValidateAuthSchemaOnStartup(gStartupAuthSchemaValidation);

        // Auto-enable auth unless we are in mixed auth/no-auth or clusterAuthMode was not provided.
        // clusterAuthMode defaults to "keyFile" if a --keyFile parameter is provided.
        if (clusterAuthMode.isDefined() && !serverGlobalParams.transitionToAuth) {
            authzManager->setAuthEnabled(true);
        }
        AuthorizationManager::set(service, std::move(authzManager));

        if (clusterAuthMode.allowsKeyFile()) {
            // Load up the keys if we allow key files authentication.
            const auto gotKeys = setUpSecurityKey(serverGlobalParams.keyFile, clusterAuthMode);
            uassert(5579201, "Unable to acquire security key[s]", gotKeys);
        }

        if (clusterAuthMode.sendsX509()) {
        // Send x509 authentication if we can.
#ifdef MONGO_CONFIG_SSL
            auth::setInternalUserAuthParams(auth::createInternalX509AuthCredential(
                boost::optional<std::string_view>{SSLManagerCoordinator::get()
                                                      ->getSSLManager()
                                                      ->getSSLConfiguration()
                                                      .clientSubjectName.toString()}));
#endif
        }
    },
    [](Service* service) { AuthorizationManager::set(service, nullptr); });

}  // namespace

void AuthzVersionParameter::append(OperationContext* opCtx,
                                   BSONObjBuilder* b,
                                   std::string_view name,
                                   const boost::optional<TenantId>&) {
    b->append(name, AuthorizationManager::schemaVersion28SCRAM);
}

Status AuthzVersionParameter::setFromString(std::string_view newValueString,
                                            const boost::optional<TenantId>&) {
    return {ErrorCodes::InternalError, "set called on unsettable server parameter"};
}

}  // namespace mongo
