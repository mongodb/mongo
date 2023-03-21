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

#include "mongo/db/commands/authentication_commands.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/authentication_commands_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/session.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

constexpr auto kExternalDB = "$external"_sd;
constexpr auto kDBFieldName = "db"_sd;

/**
 * A simple class to track "global" parameters related to the logout command.
 */
class LogoutCommandState {
public:
    /**
     * Marks the command as invoked and returns if it was previously invoked.
     */
    bool markAsInvoked() {
        return _hasBeenInvoked.swap(true);
    }

private:
    AtomicWord<bool> _hasBeenInvoked{false};
};

auto getLogoutCommandState = ServiceContext::declareDecoration<LogoutCommandState>();

class CmdLogout : public TypedCommand<CmdLogout> {
public:
    using Request = LogoutCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "de-authenticate";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext*) const final {}

        static constexpr auto kAdminDB = "admin"_sd;
        static constexpr auto kLocalDB = "local"_sd;
        void typedRun(OperationContext* opCtx) {
            auto& logoutState = getLogoutCommandState(opCtx->getServiceContext());
            auto hasBeenInvoked = logoutState.markAsInvoked();
            if (!hasBeenInvoked) {
                LOGV2_WARNING(5626600,
                              "The logout command has been deprecated, clients should end their "
                              "session instead");
            }

            auto dbname = request().getDbName();
            auto* as = AuthorizationSession::get(opCtx->getClient());

            as->logoutDatabase(
                opCtx->getClient(), dbname.toStringWithTenantId(), "Logging out on user request");
            if (getTestCommandsEnabled() && (dbname == kAdminDB)) {
                // Allows logging out as the internal user against the admin database, however
                // this actually logs out of the local database as well. This is to
                // support the auth passthrough test framework on mongos (since you can't use the
                // local database on a mongos, so you can't logout as the internal user
                // without this).
                as->logoutDatabase(opCtx->getClient(),
                                   kLocalDB,
                                   "Logging out from local database for test purposes");
            }
        }
    };
} cmdLogout;

#ifdef MONGO_CONFIG_SSL
}  // namespace

UserRequest getX509UserRequest(OperationContext* opCtx, UserRequest request) {
    std::shared_ptr<transport::Session> session;
    if (opCtx && opCtx->getClient()) {
        session = opCtx->getClient()->session();
    }

    if (!allowRolesFromX509Certificates || !session || request.roles) {
        return request;
    }

    auto& sslPeerInfo = SSLPeerInfo::forSession(session);
    auto&& peerRoles = sslPeerInfo.roles();
    if (peerRoles.empty() || (sslPeerInfo.subjectName().toString() != request.name.getUser())) {
        return request;
    }

    // In order to be hashable, the role names must be converted from unordered_set to a set.
    request.roles = std::set<RoleName>();
    std::copy(
        peerRoles.begin(), peerRoles.end(), std::inserter(*request.roles, request.roles->begin()));
    return request;
}

namespace {
constexpr auto kX509AuthenticationDisabledMessage = "x.509 authentication is disabled."_sd;

/**
 * Completes the authentication of "user".
 *
 * Returns Status::OK() on success.  All other statuses indicate failed authentication.  The
 * entire status returned here may always be used for logging.  However, if the code is
 * AuthenticationFailed, the "reason" field of the return status may contain information
 * that should not be revealed to the connected client.
 *
 * Other than AuthenticationFailed, common returns are BadValue, indicating unsupported
 * mechanism, and ProtocolError, indicating an error in the use of the authentication
 * protocol.
 */
void _authenticateX509(OperationContext* opCtx, AuthenticationSession* session) {
    auto client = opCtx->getClient();

    auto& sslPeerInfo = SSLPeerInfo::forSession(client->session());
    auto clientName = sslPeerInfo.subjectName();
    uassert(ErrorCodes::AuthenticationFailed,
            "No verified subject name available from client",
            !clientName.empty());

    UserName userName = ([&] {
        if (session->getUserName().empty()) {
            auto user = UserName(clientName.toString(), session->getDatabase().toString());
            session->updateUserName(user, true /* isMechX509 */);
            return user;
        } else {
            uassert(ErrorCodes::AuthenticationFailed,
                    "There is no x.509 client certificate matching the user.",
                    session->getUserName() == clientName.toString());
            return UserName(session->getUserName().toString(), session->getDatabase().toString());
        }
    })();

    uassert(ErrorCodes::ProtocolError,
            "SSL support is required for the MONGODB-X509 mechanism.",
            opCtx->getClient()->session()->getSSLManager());

    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);

    auto sslConfiguration = opCtx->getClient()->session()->getSSLManager()->getSSLConfiguration();

    uassert(ErrorCodes::AuthenticationFailed,
            "Unable to verify x.509 certificate, as no CA has been provided.",
            sslConfiguration.hasCA);

    uassert(ErrorCodes::ProtocolError,
            "X.509 authentication must always use the $external database.",
            userName.getDB() == kExternalDB);

    auto isInternalClient = [&]() -> bool {
        return opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient;
    };

    const auto clusterAuthMode = ClusterAuthMode::get(opCtx->getServiceContext());

    auto request = getX509UserRequest(opCtx, UserRequest(userName, boost::none));
    auto authorizeExternalUser = [&] {
        uassert(ErrorCodes::BadValue,
                kX509AuthenticationDisabledMessage,
                !isX509AuthDisabled(opCtx->getServiceContext()));
        uassertStatusOK(authorizationSession->addAndAuthorizeUser(opCtx, request, boost::none));
    };

    const bool isClusterMember = ([&] {
        const auto& requiredValue = sslGlobalParams.clusterAuthX509ExtensionValue;
        if (requiredValue.empty()) {
            return sslConfiguration.isClusterMember(clientName);
        }
        return sslPeerInfo.getClusterMembership() == requiredValue;
    })();

    if (isClusterMember) {
        // Handle internal cluster member auth, only applies to server-server connections
        if (!clusterAuthMode.allowsX509()) {
            uassert(ErrorCodes::AuthenticationFailed,
                    "The provided certificate can only be used for cluster authentication, not "
                    "client authentication. The current configuration does not allow x.509 "
                    "cluster authentication, check the --clusterAuthMode flag",
                    !gEnforceUserClusterSeparation);

            authorizeExternalUser();
        } else {
            if (!isInternalClient()) {
                LOGV2_WARNING(
                    20430,
                    "Client isn't a mongod or mongos, but is connecting with a certificate "
                    "with cluster membership");
            }

            if (gEnforceUserClusterSeparation &&
                !sslGlobalParams.clusterAuthX509ExtensionValue.empty()) {
                auto* am = AuthorizationManager::get(opCtx->getServiceContext());
                BSONObj ignored;
                const bool userExists =
                    am->getUserDescription(opCtx, request.name, &ignored).isOK();
                uassert(ErrorCodes::AuthenticationFailed,
                        "The provided certificate represents both a cluster member and an "
                        "explicit user which exists in the authzn database. "
                        "Prohibiting authentication due to enforceUserClusterSeparation setting.",
                        !userExists);
            }

            session->setAsClusterMember();
            authorizationSession->grantInternalAuthorization(client);
        }
    } else {
        // Handle normal client authentication, only applies to client-server connections
        authorizeExternalUser();
    }
}
#endif  // MONGO_CONFIG_SSL

void _authenticate(OperationContext* opCtx, AuthenticationSession* session, StringData mechanism) {
#ifdef MONGO_CONFIG_SSL
    if (mechanism == kX509AuthMechanism) {
        return _authenticateX509(opCtx, session);
    }
#endif
    uasserted(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
}

AuthenticateReply authCommand(OperationContext* opCtx,
                              AuthenticationSession* session,
                              const AuthenticateCommand& cmd) {
    auto client = opCtx->getClient();

    auto dbname = cmd.getDbName();
    auto user = cmd.getUser().value_or("");

    auto mechanism = cmd.getMechanism();

    if (!serverGlobalParams.quiet.load()) {
        LOGV2_DEBUG(5315501,
                    2,
                    "Authenticate Command",
                    "client"_attr = client->getRemote(),
                    "mechanism"_attr = mechanism,
                    "user"_attr = user,
                    logAttrs(dbname));
    }

    session->metrics()->restart();

    auto& internalSecurityUser = (*internalSecurity.getUser())->getName();
    if (getTestCommandsEnabled() && dbname.db() == "admin" &&
        user == internalSecurityUser.getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        session->updateUserName(internalSecurityUser, mechanism == auth::kMechanismMongoX509);
    } else {
        session->updateUserName(UserName{user, dbname}, mechanism == auth::kMechanismMongoX509);
    }

    if (mechanism.empty()) {
        uasserted(ErrorCodes::BadValue, "Auth mechanism not specified");
    }

    session->setMechanismName(mechanism);

    _authenticate(opCtx, session, mechanism);

    session->markSuccessful();

    AuthenticateReply reply;
    reply.setUser(session->getUserName());
    reply.setDbname(session->getDatabase());

    return reply;
}

class CmdAuthenticate final : public AuthenticateCmdVersion1Gen<CmdAuthenticate> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final try {
            return AuthenticationSession::doStep(
                opCtx, AuthenticationSession::StepType::kAuthenticate, [&](auto session) {
                    CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
                    return authCommand(opCtx, session, request());
                });
        } catch (const DBException& ex) {
            switch (ex.code()) {
                case ErrorCodes::UserNotFound:
                case ErrorCodes::ProtocolError:
                    throw;
                default:
                    uasserted(AuthorizationManager::authenticationFailedStatus.code(),
                              AuthorizationManager::authenticationFailedStatus.reason());
            }
        }
    };

    bool requiresAuth() const final {
        return false;
    }

    HandshakeRole handshakeRole() const final {
        return HandshakeRole::kAuth;
    }

} cmdAuthenticate;

}  // namespace

void doSpeculativeAuthenticate(OperationContext* opCtx,
                               BSONObj cmdObj,
                               BSONObjBuilder* result) try {

    // TypedCommands expect DB overrides in the "$db" field,
    // but coming from the Hello command has it in the "db" field.
    // Rewrite it for handling here.
    BSONObjBuilder cmd;
    bool hasDBField = false;
    for (const auto& elem : cmdObj) {
        if (elem.fieldName() == kDBFieldName) {
            cmd.appendAs(elem, AuthenticateCommand::kDbNameFieldName);
            hasDBField = true;
        } else {
            cmd.append(elem);
        }
    }

    if (!hasDBField) {
        // No "db" field was provided, so default to "$external"
        cmd.append(AuthenticateCommand::kDbNameFieldName, kExternalDB);
    }

    auto authCmdObj =
        AuthenticateCommand::parse(IDLParserContext("speculative X509 Authenticate"), cmd.obj());

    AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSpeculativeAuthenticate, [&](auto session) {
            auto authReply = authCommand(opCtx, session, authCmdObj);
            result->append(auth::kSpeculativeAuthenticate, authReply.toBSON());
        });
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
