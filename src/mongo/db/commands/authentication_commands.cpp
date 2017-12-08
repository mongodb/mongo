/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/commands/authentication_commands.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/text.h"

namespace mongo {

using std::hex;
using std::string;
using std::stringstream;

static bool _isX509AuthDisabled;
static const char _nonceAuthenticationDisabledMessage[] =
    "Challenge-response authentication using getnonce and authenticate commands is disabled.";
static const char _x509AuthenticationDisabledMessage[] = "x.509 authentication is disabled.";

void CmdAuthenticate::disableAuthMechanism(std::string authMechanism) {
    if (authMechanism == "MONGODB-X509") {
        _isX509AuthDisabled = true;
    }
}

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
    CmdGetNonce() : BasicCommand("getnonce"), _random(SecureRandom::create()) {}

    bool slaveOk() const final {
        return true;
    }

    void help(stringstream& h) const final {
        h << "internal";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) final {
        // No auth required since this command was explicitly part
        // of an authentication workflow.
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        auto n = getNextNonce();
        stringstream ss;
        ss << hex << n;
        result.append("nonce", ss.str());
        return true;
    }

private:
    int64_t getNextNonce() {
        stdx::lock_guard<SimpleMutex> lk(_randMutex);
        return _random->nextInt64();
    }

    SimpleMutex _randMutex;  // Synchronizes accesses to _random.
    std::unique_ptr<SecureRandom> _random;
} cmdGetNonce;

bool CmdAuthenticate::run(OperationContext* opCtx,
                          const string& dbname,
                          const BSONObj& cmdObj,
                          BSONObjBuilder& result) {
    if (!serverGlobalParams.quiet.load()) {
        mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
        log() << " authenticate db: " << dbname << " " << cmdToLog;
    }
    std::string mechanism = cmdObj.getStringField("mechanism");
    if (mechanism.empty()) {
        appendCommandStatus(result, {ErrorCodes::BadValue, "Auth mechanism not specified"});
        return false;
    }
    UserName user;
    auto& sslPeerInfo = SSLPeerInfo::forSession(opCtx->getClient()->session());
    if (mechanism == "MONGODB-X509" && !cmdObj.hasField("user")) {
        user = UserName(sslPeerInfo.subjectName, dbname);
    } else {
        user = UserName(cmdObj.getStringField("user"), dbname);
    }

    if (Command::testCommandsEnabled && user.getDB() == "admin" &&
        user.getUser() == internalSecurity.user->getName().getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        user = internalSecurity.user->getName();
    }

    Status status = _authenticate(opCtx, mechanism, user, cmdObj);
    audit::logAuthentication(Client::getCurrent(), mechanism, user, status.code());
    if (!status.isOK()) {
        if (!serverGlobalParams.quiet.load()) {
            auto const client = opCtx->getClient();
            log() << "Failed to authenticate " << user
                  << (client->hasRemote() ? (" from client " + client->getRemote().toString()) : "")
                  << " with mechanism " << mechanism << ": " << status;
        }
        if (status.code() == ErrorCodes::AuthenticationFailed) {
            // Statuses with code AuthenticationFailed may contain messages we do not wish to
            // reveal to the user, so we return a status with the message "auth failed".
            appendCommandStatus(result, Status(ErrorCodes::AuthenticationFailed, "auth failed"));
        } else {
            appendCommandStatus(result, status);
        }
        sleepmillis(saslGlobalParams.authFailedDelay.load());
        return false;
    }
    result.append("dbname", user.getDB());
    result.append("user", user.getUser());
    return true;
}

Status CmdAuthenticate::_authenticate(OperationContext* opCtx,
                                      const std::string& mechanism,
                                      const UserName& user,
                                      const BSONObj& cmdObj) {
#ifdef MONGO_CONFIG_SSL
    if (mechanism == "MONGODB-X509") {
        return _authenticateX509(opCtx, user, cmdObj);
    }
#endif
    return Status(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
}

#ifdef MONGO_CONFIG_SSL
Status CmdAuthenticate::_authenticateX509(OperationContext* opCtx,
                                          const UserName& user,
                                          const BSONObj& cmdObj) {
    if (!getSSLManager()) {
        return Status(ErrorCodes::ProtocolError,
                      "SSL support is required for the MONGODB-X509 mechanism.");
    }
    if (user.getDB() != "$external") {
        return Status(ErrorCodes::ProtocolError,
                      "X.509 authentication must always use the $external database.");
    }

    Client* client = Client::getCurrent();
    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);
    auto clientName = SSLPeerInfo::forSession(client->session()).subjectName;

    if (!getSSLManager()->getSSLConfiguration().hasCA) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "Unable to verify x.509 certificate, as no CA has been provided.");
    } else if (user.getUser() != clientName) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "There is no x.509 client certificate matching the user.");
    } else {
        // Handle internal cluster member auth, only applies to server-server connections
        if (getSSLManager()->getSSLConfiguration().isClusterMember(clientName)) {
            int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
            if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_undefined ||
                clusterAuthMode == ServerGlobalParams::ClusterAuthMode_keyFile) {
                return Status(ErrorCodes::AuthenticationFailed,
                              "The provided certificate "
                              "can only be used for cluster authentication, not client "
                              "authentication. The current configuration does not allow "
                              "x.509 cluster authentication, check the --clusterAuthMode flag");
            }
            authorizationSession->grantInternalAuthorization();
        }
        // Handle normal client authentication, only applies to client-server connections
        else {
            if (_isX509AuthDisabled) {
                return Status(ErrorCodes::BadValue, _x509AuthenticationDisabledMessage);
            }
            Status status = authorizationSession->addAndAuthorizeUser(opCtx, user);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }
}
#endif
CmdAuthenticate cmdAuthenticate;

class CmdLogout : public BasicCommand {
public:
    virtual bool slaveOk() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    void help(stringstream& h) const {
        h << "de-authenticate";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    CmdLogout() : BasicCommand("logout") {}
    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        AuthorizationSession* authSession = AuthorizationSession::get(Client::getCurrent());
        authSession->logoutDatabase(dbname);
        if (Command::testCommandsEnabled && dbname == "admin") {
            // Allows logging out as the internal user against the admin database, however
            // this actually logs out of the local database as well. This is to
            // support the auth passthrough test framework on mongos (since you can't use the
            // local database on a mongos, so you can't logout as the internal user
            // without this).
            authSession->logoutDatabase("local");
        }
        return true;
    }
} cmdLogout;
}
