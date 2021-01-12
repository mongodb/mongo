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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

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
#include "mongo/db/audit.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/authentication_commands_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/text.h"

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

class CmdLogout : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    std::string help() const override {
        return "de-authenticate";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    CmdLogout() : BasicCommand("logout") {}
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        AuthorizationSession* authSession = AuthorizationSession::get(Client::getCurrent());
        authSession->logoutDatabase(opCtx, dbname);
        if (getTestCommandsEnabled() && dbname == "admin") {
            // Allows logging out as the internal user against the admin database, however
            // this actually logs out of the local database as well. This is to
            // support the auth passthrough test framework on mongos (since you can't use the
            // local database on a mongos, so you can't logout as the internal user
            // without this).
            authSession->logoutDatabase(opCtx, "local");
        }
        return true;
    }
} cmdLogout;

bool _isX509AuthDisabled;

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
void _authenticateX509(OperationContext* opCtx, UserName& user, StringData dbname) {
    if (user.getUser().empty()) {
        auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
        user = UserName(sslPeerInfo.subjectName.toString(), dbname);
    }

    uassert(ErrorCodes::ProtocolError,
            "SSL support is required for the MONGODB-X509 mechanism.",
            opCtx->getClient()->session()->getSSLManager());

    uassert(ErrorCodes::ProtocolError,
            "X.509 authentication must always use the $external database.",
            user.getDB() == "$external");

    Client* client = Client::getCurrent();
    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);
    auto clientName = SSLPeerInfo::forSession(client->session()).subjectName;

    uassert(ErrorCodes::AuthenticationFailed,
            "No verified subject name available from client",
            !clientName.empty());

    auto sslConfiguration = opCtx->getClient()->session()->getSSLConfiguration();

    uassert(ErrorCodes::AuthenticationFailed,
            "Unable to verify x.509 certificate, as no CA has been provided.",
            sslConfiguration->hasCA);

    uassert(ErrorCodes::AuthenticationFailed,
            "There is no x.509 client certificate matching the user.",
            user.getUser() == clientName.toString());

    // Handle internal cluster member auth, only applies to server-server connections
    if (sslConfiguration->isClusterMember(clientName)) {
        uassertStatusOK(authCounter.incClusterAuthenticateReceived("MONGODB-X509"));

        int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();

        uassert(ErrorCodes::AuthenticationFailed,
                "The provided certificate "
                "can only be used for cluster authentication, not client "
                "authentication. The current configuration does not allow "
                "x.509 cluster authentication, check the --clusterAuthMode flag",
                clusterAuthMode != ServerGlobalParams::ClusterAuthMode_undefined &&
                    clusterAuthMode != ServerGlobalParams::ClusterAuthMode_keyFile);

        if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
            auto clientMetadataDoc = clientMetadata->getDocument();
            auto driverName = clientMetadataDoc.getObjectField("driver"_sd)
                                  .getField("name"_sd)
                                  .checkAndGetStringData();
            if (!clientMetadata->getApplicationName().empty() ||
                (driverName != "MongoDB Internal Client" && driverName != "NetworkInterfaceTL")) {
                LOGV2_WARNING(20430,
                              "Client isn't a mongod or mongos, but is connecting with a "
                              "certificate with cluster membership");
            }


            uassertStatusOK(authCounter.incClusterAuthenticateSuccessful("MONGODB-X509"));
        }

        authorizationSession->grantInternalAuthorization(client);
    } else {
        // Handle normal client authentication, only applies to client-server connections
        uassert(ErrorCodes::BadValue, kX509AuthenticationDisabledMessage, !_isX509AuthDisabled);
        uassertStatusOK(authorizationSession->addAndAuthorizeUser(opCtx, user));
    }
}
#endif  // MONGO_CONFIG_SSL

void _authenticate(OperationContext* opCtx,
                   StringData mechanism,
                   UserName& user,
                   StringData dbname) {
#ifdef MONGO_CONFIG_SSL
    if (mechanism == kX509AuthMechanism) {
        return _authenticateX509(opCtx, user, dbname);
    }
#endif
    uasserted(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
}

AuthenticateReply authCommand(OperationContext* opCtx, const AuthenticateCommand& cmd) {
    auto dbname = cmd.getDbName();
    UserName user(cmd.getUser().value_or(""), dbname);
    const std::string mechanism(cmd.getMechanism());

    if (!serverGlobalParams.quiet.load()) {
        LOGV2_DEBUG(5315501,
                    2,
                    "Authenticate Command",
                    "db"_attr = dbname,
                    "user"_attr = user,
                    "mechanism"_attr = mechanism);
    }

    if (mechanism.empty()) {
        uasserted(ErrorCodes::BadValue, "Auth mechanism not specified");
    }

    if (getTestCommandsEnabled() && user.getDB() == "admin" &&
        user.getUser() == internalSecurity.user->getName().getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        user = internalSecurity.user->getName();
    }

    try {
        uassertStatusOK(authCounter.incAuthenticateReceived(mechanism));

        _authenticate(opCtx, mechanism, user, dbname);
        audit::logAuthentication(opCtx->getClient(), mechanism, user, ErrorCodes::OK);

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20429,
                  "Successfully authenticated as principal {user} on {db} from client {client}",
                  "Successfully authenticated",
                  "user"_attr = user.getUser(),
                  "db"_attr = user.getDB(),
                  "remote"_attr = opCtx->getClient()->session()->remote());
        }

        uassertStatusOK(authCounter.incAuthenticateSuccessful(mechanism));

        return AuthenticateReply(user.getUser().toString(), user.getDB().toString());

    } catch (const AssertionException& ex) {
        auto status = ex.toStatus();
        auto const client = opCtx->getClient();
        audit::logAuthentication(client, mechanism, user, status.code());
        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20428,
                  "Failed to authenticate",
                  "user"_attr = user,
                  "client"_attr = client->getRemote(),
                  "mechanism"_attr = mechanism,
                  "error"_attr = status);
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

        void doCheckAuthorization(OperationContext*) const final {}

        Reply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            return authCommand(opCtx, request());
        }
    };

    bool requiresAuth() const final {
        return false;
    }

} cmdAuthenticate;

}  // namespace

void disableAuthMechanism(StringData authMechanism) {
    if (authMechanism == kX509AuthMechanism) {
        _isX509AuthDisabled = true;
    }
}

void doSpeculativeAuthenticate(OperationContext* opCtx,
                               BSONObj cmdObj,
                               BSONObjBuilder* result) try {

    // TypedCommands expect DB overrides in the "$db" field,
    // but coming from the Hello command has it in the "db" field.
    // Rewrite it for handling here.
    BSONObjBuilder cmd;
    for (const auto& elem : cmdObj) {
        if (elem.fieldName() != kDBFieldName) {
            cmd.append(elem);
        }
    }

    cmd.append(AuthenticateCommand::kDbNameFieldName, kExternalDB);

    auto authCmdObj = AuthenticateCommand::parse(
        IDLParserErrorContext("speculative X509 Authenticate"), cmd.obj());


    const auto mechanism = authCmdObj.getMechanism().toString();

    // Run will make sure an audit entry happens. Let it reach that point.
    authCounter.incSpeculativeAuthenticateReceived(mechanism).ignore();

    auto authReply = authCommand(opCtx, authCmdObj);

    uassertStatusOK(authCounter.incSpeculativeAuthenticateSuccessful(mechanism));
    result->append(auth::kSpeculativeAuthenticate, authReply.toBSON());
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
