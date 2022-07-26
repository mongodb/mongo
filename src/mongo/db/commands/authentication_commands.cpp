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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/authentication_commands_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/user_management_commands_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/text.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

constexpr auto kExternalDB = "$external"_sd;
constexpr auto kDBFieldName = "db"_sd;

/**
 * Returns a random 64-bit nonce.
 *
 * Previously, this command would have been called prior to {authenticate: ...}
 * when using the MONGODB-CR authentication mechanism.
 * Since that mechanism has been removed from MongoDB 3.8,
 * it is nominally no longer required.
 *
 * Unfortunately, mongo-tools uses a connection library
 * which optimistically invokes {getnonce: 1} upon connection
 * under the assumption that it will eventually be used as part
 * of "classic" authentication.
 * If the command dissapeared, then all of mongo-tools would
 * fail to connect, despite using SCRAM-SHA-1 or another valid
 * auth mechanism. Thus, we have to keep this command around for now.
 *
 * Note that despite nonces being available, they are not bound
 * to the AuthorizationSession anymore, and the authenticate
 * command doesn't acknowledge their existence.
 */
class CmdGetNonce : public BasicCommand {
public:
    CmdGetNonce() : BasicCommand("getnonce") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "internal";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    bool requiresAuth() const override {
        return false;
    }
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {
        // No auth required since this command was explicitly part
        // of an authentication workflow.
    }

    bool run(OperationContext* opCtx,
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        auto n = getNextNonce();
        std::stringstream ss;
        ss << std::hex << n;
        result.append("nonce", ss.str());
        return true;
    }

private:
    int64_t getNextNonce() {
        stdx::lock_guard<SimpleMutex> lk(_randMutex);
        return _random.nextInt64();
    }

    SimpleMutex _randMutex;  // Synchronizes accesses to _random.
    SecureRandom _random;
} cmdGetNonce;

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

            as->logoutDatabase(opCtx->getClient(), dbname, "Logging out on user request");
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
    auto clientName = sslPeerInfo.subjectName;
    uassert(ErrorCodes::AuthenticationFailed,
            "No verified subject name available from client",
            !clientName.empty());

    auto user = [&] {
        if (session->getUserName().empty()) {
            auto user = UserName(clientName.toString(), session->getDatabase().toString());
            session->updateUserName(user);
            return user;
        } else {
            uassert(ErrorCodes::AuthenticationFailed,
                    "There is no x.509 client certificate matching the user.",
                    session->getUserName() == clientName.toString());
            return UserName(session->getUserName().toString(), session->getDatabase().toString());
        }
    }();

    uassert(ErrorCodes::ProtocolError,
            "SSL support is required for the MONGODB-X509 mechanism.",
            opCtx->getClient()->session()->getSSLManager());

    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);

    auto sslConfiguration = opCtx->getClient()->session()->getSSLConfiguration();

    uassert(ErrorCodes::AuthenticationFailed,
            "Unable to verify x.509 certificate, as no CA has been provided.",
            sslConfiguration->hasCA);

    uassert(ErrorCodes::ProtocolError,
            "X.509 authentication must always use the $external database.",
            user.getDB() == kExternalDB);

    auto isInternalClient = [&]() -> bool {
        return opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient;
    };

    const auto clusterAuthMode = ClusterAuthMode::get(opCtx->getServiceContext());

    auto authorizeExternalUser = [&] {
        uassert(ErrorCodes::BadValue,
                kX509AuthenticationDisabledMessage,
                !isX509AuthDisabled(opCtx->getServiceContext()));
        uassertStatusOK(authorizationSession->addAndAuthorizeUser(opCtx, user));
    };

    if (sslConfiguration->isClusterMember(clientName)) {
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
                    "db"_attr = dbname);
    }

    auto& internalSecurityUser = (*internalSecurity.getUser())->getName();
    if (getTestCommandsEnabled() && dbname == "admin" && user == internalSecurityUser.getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        session->updateUserName(internalSecurityUser);
    } else {
        session->updateUserName(UserName{user, dbname});
    }

    if (mechanism.empty()) {
        uasserted(ErrorCodes::BadValue, "Auth mechanism not specified");
    }
    session->setMechanismName(mechanism);

    try {
        _authenticate(opCtx, session, mechanism);
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20429,
                  "Successfully authenticated",
                  "client"_attr = client->getRemote(),
                  "mechanism"_attr = mechanism,
                  "user"_attr = session->getUserName(),
                  "db"_attr = session->getDatabase());
        }

        session->markSuccessful();

        AuthenticateReply reply;
        reply.setUser(session->getUserName());
        reply.setDbname(session->getDatabase());
        return reply;

    } catch (const AssertionException& ex) {
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20428,
                  "Failed to authenticate",
                  "client"_attr = client->getRemote(),
                  "mechanism"_attr = mechanism,
                  "user"_attr = session->getUserName(),
                  "db"_attr = session->getDatabase(),
                  "error"_attr = ex.toStatus());
        }

        throw;
    }
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

        Reply typedRun(OperationContext* opCtx) final {
            return AuthenticationSession::doStep(
                opCtx, AuthenticationSession::StepType::kAuthenticate, [&](auto session) {
                    CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
                    return authCommand(opCtx, session, request());
                });
        }
    };

    bool requiresAuth() const final {
        return false;
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
