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


#include "mongo/db/commands/authentication_commands.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/authenticate.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/sasl_commands.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_payload.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_request_x509.h"
#include "mongo/db/auth/x509_protocol_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/authentication_commands_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

constexpr auto kDBFieldName = "db"_sd;
constexpr auto kSASLPayloadUsernameField = "username"_sd;
constexpr StringData kX509AuthMechanism = "MONGODB-X509"_sd;

class CmdLogout : public TypedCommand<CmdLogout> {
public:
    using Request = LogoutCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "de-authenticate";
    }

    // We should allow users to logout even if the user does not have the direct shard roles action
    // type.
    bool shouldSkipDirectConnectionChecks() const final {
        return true;
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

        void typedRun(OperationContext* opCtx) {
            static std::once_flag logoutState;
            std::call_once(logoutState, []() {
                LOGV2_WARNING(5626600,
                              "The logout command has been deprecated, clients should end their "
                              "session instead");
            });

            auto dbname = request().getDbName();
            auto* as = AuthorizationSession::get(opCtx->getClient());

            as->logoutDatabase(dbname, "Logging out on user request");
            if (getTestCommandsEnabled() && (dbname.isAdminDB())) {
                // Allows logging out as the internal user against the admin database, however
                // this actually logs out of the local database as well. This is to
                // support the auth passthrough test framework on mongos (since you can't use the
                // local database on a mongos, so you can't logout as the internal user
                // without this).
                as->logoutDatabase(
                    DatabaseNameUtil::deserialize(dbname.tenantId(),
                                                  DatabaseName::kLocal.db(omitTenant),
                                                  request().getSerializationContext()),
                    "Logging out from local database for test purposes");
            }
        }
    };
};
MONGO_REGISTER_COMMAND(CmdLogout).forRouter().forShard();

#ifdef MONGO_CONFIG_SSL

std::unique_ptr<UserRequest> getX509UserRequest(OperationContext* opCtx, const UserName& username) {
    std::shared_ptr<transport::Session> session;
    if (opCtx && opCtx->getClient()) {
        session = opCtx->getClient()->session();
    }

    if (!allowRolesFromX509Certificates || !session) {
        return std::make_unique<UserRequestGeneral>(username, boost::none);
    }

    auto sslPeerInfo = SSLPeerInfo::forSession(session);
    if (!sslPeerInfo || sslPeerInfo->roles().empty() ||
        (sslPeerInfo->subjectName().toString() != username.getUser())) {
        return std::make_unique<UserRequestGeneral>(username, boost::none);
    }

    auto peerRoles = sslPeerInfo->roles();
    auto roles = std::set<RoleName>();
    std::copy(peerRoles.begin(), peerRoles.end(), std::inserter(roles, roles.begin()));

    return uassertStatusOK(UserRequestX509::makeUserRequestX509(username, roles, sslPeerInfo));
}

constexpr auto kX509AuthenticationDisabledMessage = "x.509 authentication is disabled."_sd;

// TODO SERVER-78809: remove
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

    auto sslPeerInfo = SSLPeerInfo::forSession(client->session());
    uassert(ErrorCodes::AuthenticationFailed, "No SSLPeerInfo available", sslPeerInfo);
    auto clientName = sslPeerInfo->subjectName();
    uassert(ErrorCodes::AuthenticationFailed,
            "No verified subject name available from client",
            !clientName.empty());

    UserName userName = ([&] {
        if (session->getUserName().empty()) {
            auto user = UserName(clientName.toString(), std::string{session->getDatabase()});
            session->updateUserName(user, true /* isMechX509 */);
            return user;
        } else {
            uassert(ErrorCodes::AuthenticationFailed,
                    "There is no x.509 client certificate matching the user.",
                    session->getUserName() == clientName.toString());
            return UserName(std::string{session->getUserName()},
                            std::string{session->getDatabase()});
        }
    })();

    uassert(ErrorCodes::ProtocolError,
            "SSL support is required for the MONGODB-X509 mechanism.",
            opCtx->getClient()->session()->getSSLConfiguration());

    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);

    auto sslConfiguration = opCtx->getClient()->session()->getSSLConfiguration();

    uassert(ErrorCodes::ProtocolError,
            "X.509 authentication must always use the $external database.",
            userName.getDatabaseName().isExternalDB());

    const auto clusterAuthMode = ClusterAuthMode::get(opCtx->getServiceContext());

    auto request = getX509UserRequest(opCtx, userName);

    auto authorizeExternalUser = [&] {
        uassert(ErrorCodes::BadValue,
                kX509AuthenticationDisabledMessage,
                sequenceContains(saslGlobalParams.authenticationMechanisms, kX509AuthMechanism));

        uassertStatusOK(
            authorizationSession->addAndAuthorizeUser(opCtx, std::move(request), boost::none));
    };

    if (sslConfiguration->isClusterMember(clientName, sslPeerInfo->getClusterMembership())) {
        // Handle internal cluster member auth, only applies to server-server connections
        if (!clusterAuthMode.allowsX509()) {
            uassert(ErrorCodes::AuthenticationFailed,
                    "The provided certificate can only be used for cluster authentication, not "
                    "client authentication. The current configuration does not allow x.509 "
                    "cluster authentication, check the --clusterAuthMode flag",
                    !gEnforceUserClusterSeparation);

            authorizeExternalUser();
        } else {
            if (!opCtx->getClient()->isPossiblyUnauthenticatedInternalClient()) {
                LOGV2_WARNING(
                    20430,
                    "Client isn't a mongod or mongos, but is connecting with a certificate "
                    "with cluster membership");
            }

            if (gEnforceUserClusterSeparation && sslConfiguration->isClusterExtensionSet()) {
                auto* am = AuthorizationManager::get(opCtx->getService());
                BSONObj ignored;

                // The UserRequest here should represent the X.509 subject DN, NOT local.__system.
                // This ensures that we are checking for the presence of a user matching the X.509
                // subject rather than __system (which should always exist).
                bool userExists = am->acquireUser(opCtx, request->clone()).isOK();
                uassert(ErrorCodes::AuthenticationFailed,
                        "The provided certificate represents both a cluster member and an "
                        "explicit user which exists in the authzn database. "
                        "Prohibiting authentication due to enforceUserClusterSeparation setting.",
                        !userExists);
            }

            session->setAsClusterMember();
            authorizationSession->grantInternalAuthorization();
        }
    } else {
        // Handle normal client authentication, only applies to client-server connections
        authorizeExternalUser();
    }
}
#endif  // MONGO_CONFIG_SSL

// TODO SERVER-78809: remove
void _authenticate(OperationContext* opCtx, AuthenticationSession* session, StringData mechanism) {
#ifdef MONGO_CONFIG_SSL
    if (mechanism == auth::kMechanismMongoX509) {
        return _authenticateX509(opCtx, session);
    }
#endif
    uasserted(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
}

auth::SaslPayload generateSaslPayload(const boost::optional<StringData>& user,
                                      const DatabaseName& dbname) {
    auth::X509MechanismClientStep1 step;
    step.setPrincipalName(user);
    auto payloadBSON = step.toBSON();
    auto payloadStr = std::string(payloadBSON.objdata(), payloadBSON.objsize());
    return auth::SaslPayload(payloadStr);
}

std::string getNameFromPeerInfo(Client* client) {
#ifdef MONGO_CONFIG_SSL
    // If the client's subjectName is empty, that means that there is no certificate for
    // the user and they won't be able to use MONGODB-X509 anyways.
    auto sslPeerInfo = SSLPeerInfo::forSession(client->session());
    return sslPeerInfo ? sslPeerInfo->subjectName().toString() : std::string();
#else
    uasserted(ErrorCodes::BadValue, "MONGODB-X509 is unsupported on no-ssl builds");
#endif
}

/**
 * The steps of authCommand (sans feature flag) are below.
 *
 * 1. We should validate the inputs.
 * 2. We should synthesize the SASLStartCommand payload.
 * 3. We should log that we are performing the Authenticate Command.
 * 4. We should start metrics capture.
 * 5. We should call runSASLStart with the command payload.
 * 6. We should translate the saslStartReply into an authenticate reply.
 * 7. We should return the authenticate reply.
 */
AuthenticateReply authCommand(OperationContext* opCtx,
                              AuthenticationSession* session,
                              const AuthenticateCommand& cmd) {

    auto client = opCtx->getClient();
    auto dbname = cmd.getDbName();
    auto user = cmd.getUser();
    auto mechanism = cmd.getMechanism();

    // TODO SERVER-78809: remove
    if (!gFeatureFlagRearchitectUserAcquisition.isEnabled()) {

        std::string userStr{user.value_or("")};

        if (!serverGlobalParams.quiet.load()) {
            LOGV2_DEBUG(5315501,
                        2,
                        "Authenticate Command",
                        "client"_attr = client->getRemote(),
                        "mechanism"_attr = mechanism,
                        "user"_attr = user,
                        logAttrs(dbname));
        }

        auto& internalSecurityUser = (*internalSecurity.getUser())->getName();
        if (getTestCommandsEnabled() && dbname.isAdminDB() &&
            userStr == internalSecurityUser.getUser()) {
            // Allows authenticating as the internal user against the admin database.  This is to
            // support the auth passthrough test framework on mongos (since you can't use the local
            // database on a mongos, so you can't auth as the internal user without this).
            session->updateUserName(internalSecurityUser, mechanism == auth::kMechanismMongoX509);
        } else {
            session->updateUserName(UserName{userStr, dbname},
                                    mechanism == auth::kMechanismMongoX509);
        }

        uassert(ErrorCodes::BadValue, "Auth mechanism not specified", !mechanism.empty());

        session->metrics()->restart();

        session->setMechanismName(mechanism);

        _authenticate(opCtx, session, mechanism);

        session->markSuccessful();

        AuthenticateReply reply;
        reply.setUser(session->getUserName());
        reply.setDbname(session->getDatabase());

        return reply;
    }

    // Synthesize the SASLStartCommand.
    auth::SaslStartCommand request;
    auto payload = generateSaslPayload(user, dbname);

    request.setPayload(std::move(payload));
    request.setDbName(dbname);
    request.setMechanism(mechanism);
    request.setSerializationContext(cmd.getSerializationContext());

    // Log Authenticate command.
    if (!serverGlobalParams.quiet.load()) {
        LOGV2_DEBUG(8209201,
                    2,
                    "Authenticate Command",
                    "client"_attr = client->getRemote(),
                    "mechanism"_attr = mechanism,
                    "user"_attr = user,
                    logAttrs(dbname));
    }

    // Run SASL Start.
    // We do not need to check the response to runSaslStart because that function
    // will throw and we will eventually get caught by the AuthenticationSession
    // stepguard.
    auto saslStartReply = runSaslStart(opCtx, session, request);
    invariant(saslStartReply.getDone() == true);

    // Translate SASLStartReply and return AuthenticateReply.
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
};
MONGO_REGISTER_COMMAND(CmdAuthenticate).forRouter().forShard();

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
        cmd.append(AuthenticateCommand::kDbNameFieldName, DatabaseName::kExternal.db(omitTenant));
    }

    auto authCmdObj =
        AuthenticateCommand::parse(cmd.obj(), IDLParserContext("speculative X509 Authenticate"));

    AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSpeculativeAuthenticate, [&](auto session) {
            auto authReply = authCommand(opCtx, session, authCmdObj);
            result->append(auth::kSpeculativeAuthenticate, authReply.toBSON());
        });
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
