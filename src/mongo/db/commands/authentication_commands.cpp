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
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/mongo_authentication_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/text.h"

namespace mongo {

using std::hex;
using std::string;
using std::stringstream;

static bool _isCRAuthDisabled;
static bool _isX509AuthDisabled;
static const char _nonceAuthenticationDisabledMessage[] =
    "Challenge-response authentication using getnonce and authenticate commands is disabled.";
static const char _x509AuthenticationDisabledMessage[] = "x.509 authentication is disabled.";

void CmdAuthenticate::disableAuthMechanism(std::string authMechanism) {
    if (authMechanism == "MONGODB-CR") {
        _isCRAuthDisabled = true;
    }
    if (authMechanism == "MONGODB-X509") {
        _isX509AuthDisabled = true;
    }
}

/* authentication

   system.users contains
     { user : <username>, pwd : <pwd_digest>, ... }

   getnonce sends nonce to client

   client then sends { authenticate:1, nonce64:<nonce_str>, user:<username>, key:<key> }

   where <key> is md5(<nonce_str><username><pwd_digest_str>) as a string
*/

class CmdGetNonce : public Command {
public:
    CmdGetNonce() : Command("getnonce"), _random(SecureRandom::create()) {}

    virtual bool slaveOk() const {
        return true;
    }
    void help(stringstream& h) const {
        h << "internal";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        nonce64 n = getNextNonce();
        stringstream ss;
        ss << hex << n;
        result.append("nonce", ss.str());
        AuthenticationSession::set(ClientBasic::getCurrent(),
                                   stdx::make_unique<MongoAuthenticationSession>(n));
        return true;
    }

private:
    nonce64 getNextNonce() {
        stdx::lock_guard<SimpleMutex> lk(_randMutex);
        return _random->nextInt64();
    }

    SimpleMutex _randMutex;  // Synchronizes accesses to _random.
    std::unique_ptr<SecureRandom> _random;
} cmdGetNonce;

void CmdAuthenticate::redactForLogging(mutablebson::Document* cmdObj) {
    namespace mmb = mutablebson;
    static const int numRedactedFields = 2;
    static const char* redactedFields[numRedactedFields] = {"key", "nonce"};
    for (int i = 0; i < numRedactedFields; ++i) {
        for (mmb::Element element = mmb::findFirstChildNamed(cmdObj->root(), redactedFields[i]);
             element.ok();
             element = mmb::findElementNamed(element.rightSibling(), redactedFields[i])) {
            element.setValueString("xxx");
        }
    }
}

bool CmdAuthenticate::run(OperationContext* txn,
                          const string& dbname,
                          BSONObj& cmdObj,
                          int,
                          string& errmsg,
                          BSONObjBuilder& result) {
    if (!serverGlobalParams.quiet) {
        mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
        redactForLogging(&cmdToLog);
        log() << " authenticate db: " << dbname << " " << cmdToLog;
    }

    UserName user(cmdObj.getStringField("user"), dbname);
    if (Command::testCommandsEnabled && user.getDB() == "admin" &&
        user.getUser() == internalSecurity.user->getName().getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        user = internalSecurity.user->getName();
    }

    std::string mechanism = cmdObj.getStringField("mechanism");
    if (mechanism.empty()) {
        mechanism = "MONGODB-CR";
    }
    Status status = _authenticate(txn, mechanism, user, cmdObj);
    audit::logAuthentication(ClientBasic::getCurrent(), mechanism, user, status.code());
    if (!status.isOK()) {
        if (!serverGlobalParams.quiet) {
            log() << "Failed to authenticate " << user << " with mechanism " << mechanism << ": "
                  << status;
        }
        if (status.code() == ErrorCodes::AuthenticationFailed) {
            // Statuses with code AuthenticationFailed may contain messages we do not wish to
            // reveal to the user, so we return a status with the message "auth failed".
            appendCommandStatus(result, Status(ErrorCodes::AuthenticationFailed, "auth failed"));
        } else {
            appendCommandStatus(result, status);
        }
        sleepmillis(saslGlobalParams.authFailedDelay);
        return false;
    }
    result.append("dbname", user.getDB());
    result.append("user", user.getUser());
    return true;
}

Status CmdAuthenticate::_authenticate(OperationContext* txn,
                                      const std::string& mechanism,
                                      const UserName& user,
                                      const BSONObj& cmdObj) {
    if (mechanism == "MONGODB-CR") {
        return _authenticateCR(txn, user, cmdObj);
    }
#ifdef MONGO_CONFIG_SSL
    if (mechanism == "MONGODB-X509") {
        return _authenticateX509(txn, user, cmdObj);
    }
#endif
    return Status(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
}

Status CmdAuthenticate::_authenticateCR(OperationContext* txn,
                                        const UserName& user,
                                        const BSONObj& cmdObj) {
    if (user == internalSecurity.user->getName() &&
        serverGlobalParams.clusterAuthMode.load() == ServerGlobalParams::ClusterAuthMode_x509) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "Mechanism x509 is required for internal cluster authentication");
    }

    if (_isCRAuthDisabled) {
        // SERVER-8461, MONGODB-CR must be enabled for authenticating the internal user, so that
        // cluster members may communicate with each other.
        if (user != internalSecurity.user->getName()) {
            return Status(ErrorCodes::BadValue, _nonceAuthenticationDisabledMessage);
        }
    }

    string key = cmdObj.getStringField("key");
    string received_nonce = cmdObj.getStringField("nonce");

    if (user.getUser().empty() || key.empty() || received_nonce.empty()) {
        sleepmillis(10);
        return Status(ErrorCodes::ProtocolError,
                      "field missing/wrong type in received authenticate command");
    }

    stringstream digestBuilder;

    {
        ClientBasic* client = ClientBasic::getCurrent();
        std::unique_ptr<AuthenticationSession> session;
        AuthenticationSession::swap(client, session);
        if (!session || session->getType() != AuthenticationSession::SESSION_TYPE_MONGO) {
            sleepmillis(30);
            return Status(ErrorCodes::ProtocolError, "No pending nonce");
        } else {
            nonce64 nonce = static_cast<MongoAuthenticationSession*>(session.get())->getNonce();
            digestBuilder << hex << nonce;
            if (digestBuilder.str() != received_nonce) {
                sleepmillis(30);
                return Status(ErrorCodes::AuthenticationFailed, "Received wrong nonce.");
            }
        }
    }

    User* userObj;
    Status status = getGlobalAuthorizationManager()->acquireUser(txn, user, &userObj);
    if (!status.isOK()) {
        // Failure to find the privilege document indicates no-such-user, a fact that we do not
        // wish to reveal to the client.  So, we return AuthenticationFailed rather than passing
        // through the returned status.
        return Status(ErrorCodes::AuthenticationFailed, status.toString());
    }
    string pwd = userObj->getCredentials().password;
    getGlobalAuthorizationManager()->releaseUser(userObj);

    if (pwd.empty()) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "MONGODB-CR credentials missing in the user document");
    }

    md5digest d;
    {
        digestBuilder << user.getUser() << pwd;
        string done = digestBuilder.str();

        md5_state_t st;
        md5_init(&st);
        md5_append(&st, (const md5_byte_t*)done.c_str(), done.size());
        md5_finish(&st, d);
    }

    string computed = digestToString(d);

    if (key != computed) {
        return Status(ErrorCodes::AuthenticationFailed, "key mismatch");
    }

    AuthorizationSession* authorizationSession =
        AuthorizationSession::get(ClientBasic::getCurrent());
    status = authorizationSession->addAndAuthorizeUser(txn, user);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

#ifdef MONGO_CONFIG_SSL
Status CmdAuthenticate::_authenticateX509(OperationContext* txn,
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

    ClientBasic* client = ClientBasic::getCurrent();
    AuthorizationSession* authorizationSession = AuthorizationSession::get(client);
    auto clientName = client->session()->getX509SubjectName();

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
            Status status = authorizationSession->addAndAuthorizeUser(txn, user);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }
}
#endif
CmdAuthenticate cmdAuthenticate;

class CmdLogout : public Command {
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
    CmdLogout() : Command("logout") {}
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        AuthorizationSession* authSession = AuthorizationSession::get(ClientBasic::getCurrent());
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
