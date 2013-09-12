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

#include "mongo/db/commands/authentication_commands.h"

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/mongo_authentication_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/net/ssl_manager.h"

namespace mongo {

    static bool _areNonceAuthenticateCommandsEnabled = true;
    static const char _nonceAuthenticateCommandsDisabledMessage[] =
        "Challenge-response authentication using getnonce and authenticate commands is disabled.";

    void CmdAuthenticate::disableCommand() { _areNonceAuthenticateCommandsEnabled = false; }

    /* authentication

       system.users contains
         { user : <username>, pwd : <pwd_digest>, ... }

       getnonce sends nonce to client

       client then sends { authenticate:1, nonce64:<nonce_str>, user:<username>, key:<key> }

       where <key> is md5(<nonce_str><username><pwd_digest_str>) as a string
    */

    class CmdGetNonce : public Command {
    public:
        CmdGetNonce() :
            Command("getnonce"),
            _randMutex("getnonce"),
            _random(SecureRandom::create()) {
        }

        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const {
            return true;
        }
        void help(stringstream& h) const { h << "internal"; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        bool run(const string&, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            nonce64 n = getNextNonce();
            stringstream ss;
            ss << hex << n;
            result.append("nonce", ss.str() );
            ClientBasic::getCurrent()->resetAuthenticationSession(
                    new MongoAuthenticationSession(n));
            return true;
        }

    private:
        nonce64 getNextNonce() {
            SimpleMutex::scoped_lock lk(_randMutex);
            return _random->nextInt64();
        }

        SimpleMutex _randMutex;  // Synchronizes accesses to _random.
        boost::scoped_ptr<SecureRandom> _random;
    } cmdGetNonce;

    bool CmdAuthenticate::run(const string& dbname,
                              BSONObj& cmdObj,
                              int,
                              string& errmsg,
                              BSONObjBuilder& result,
                              bool fromRepl) {

        mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
        redactForLogging(&cmdToLog);
        log() << " authenticate db: " << dbname << " " << cmdToLog << endl;
        UserName user(cmdObj.getStringField("user"), dbname);
        std::string mechanism = cmdObj.getStringField("mechanism");
        if (mechanism.empty()) {
            mechanism = "MONGODB-CR";
        }
        Status status = _authenticate(mechanism, user, cmdObj);
        audit::logAuthentication(ClientBasic::getCurrent(),
                                 mechanism,
                                 user,
                                 status.code());
        if (!status.isOK()) {
            log() << "Failed to authenticate " << user << " with mechanism " << mechanism << ": " <<
                status;
            if (status.code() == ErrorCodes::AuthenticationFailed) {
                // Statuses with code AuthenticationFailed may contain messages we do not wish to
                // reveal to the user, so we return a status with the message "auth failed".
                appendCommandStatus(result,
                                    Status(ErrorCodes::AuthenticationFailed, "auth failed"));
            }
            else {
                appendCommandStatus(result, status);
            }
            return false;
        }
        result.append("dbname", user.getDB());
        result.append("user", user.getUser());
        return true;
    }

    Status CmdAuthenticate::_authenticate(const std::string& mechanism,
                                          const UserName& user,
                                          const BSONObj& cmdObj) {

        if (mechanism == "MONGODB-CR") {
            return _authenticateCR(user, cmdObj);
        }
#ifdef MONGO_SSL
        if (mechanism == "MONGODB-X509") {
            return _authenticateX509(user, cmdObj);
        }
#endif
        return Status(ErrorCodes::BadValue, "Unsupported mechanism: " + mechanism);
    }

    Status CmdAuthenticate::_authenticateCR(const UserName& user, const BSONObj& cmdObj) {

        if (user == internalSecurity.user->getName() && cmdLine.clusterAuthMode == "x509") {
            return Status(ErrorCodes::AuthenticationFailed,
                          "Mechanism x509 is required for internal cluster authentication");
        }

        if (!_areNonceAuthenticateCommandsEnabled) {
            // SERVER-8461, MONGODB-CR must be enabled for authenticating the internal user, so that
            // cluster members may communicate with each other.
            if (user != internalSecurity.user->getName()) {
                return Status(ErrorCodes::BadValue, _nonceAuthenticateCommandsDisabledMessage);
            }
        }

        string key = cmdObj.getStringField("key");
        string received_nonce = cmdObj.getStringField("nonce");

        if( user.getUser().empty() || key.empty() || received_nonce.empty() ) {
            sleepmillis(10);
            return Status(ErrorCodes::ProtocolError,
                          "field missing/wrong type in received authenticate command");
        }

        stringstream digestBuilder;

        {
            ClientBasic *client = ClientBasic::getCurrent();
            boost::scoped_ptr<AuthenticationSession> session;
            client->swapAuthenticationSession(session);
            if (!session || session->getType() != AuthenticationSession::SESSION_TYPE_MONGO) {
                sleepmillis(30);
                return Status(ErrorCodes::ProtocolError, "No pending nonce");
            }
            else {
                nonce64 nonce = static_cast<MongoAuthenticationSession*>(session.get())->getNonce();
                digestBuilder << hex << nonce;
                if (digestBuilder.str() != received_nonce) {
                    sleepmillis(30);
                    return Status(ErrorCodes::AuthenticationFailed, "Received wrong nonce.");
                }
            }
        }

        User* userObj;
        Status status = getGlobalAuthorizationManager()->acquireUser(user, &userObj);
        if (!status.isOK()) {
            // Failure to find the privilege document indicates no-such-user, a fact that we do not
            // wish to reveal to the client.  So, we return AuthenticationFailed rather than passing
            // through the returned status.
            return Status(ErrorCodes::AuthenticationFailed, status.toString());
        }
        string pwd = userObj->getCredentials().password;
        getGlobalAuthorizationManager()->releaseUser(userObj);

        md5digest d;
        {
            digestBuilder << user.getUser() << pwd;
            string done = digestBuilder.str();

            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) done.c_str(), done.size());
            md5_finish(&st, d);
        }

        string computed = digestToString( d );

        if ( key != computed ) {
            return Status(ErrorCodes::AuthenticationFailed, "key mismatch");
        }

        AuthorizationSession* authorizationSession =
            ClientBasic::getCurrent()->getAuthorizationSession();
        status = authorizationSession->addAndAuthorizeUser(user);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

#ifdef MONGO_SSL
    Status CmdAuthenticate::_authenticateX509(const UserName& user, const BSONObj& cmdObj) {
        if(user.getDB() != "$external") {
            return Status(ErrorCodes::ProtocolError,
                          "X.509 authentication must always use the $external database.");
        }

        ClientBasic *client = ClientBasic::getCurrent();
        AuthorizationSession* authorizationSession = client->getAuthorizationSession();
        StringData subjectName = client->port()->getX509SubjectName();

        if (user.getUser() != subjectName) {
            return Status(ErrorCodes::AuthenticationFailed,
                          "There is no x.509 client certificate matching the user.");
        }
        else {
            StringData srvSubjectName = getSSLManager()->getServerSubjectName();
            StringData srvClusterId = srvSubjectName.substr(srvSubjectName.find(",OU="));
            StringData peerClusterId = subjectName.substr(subjectName.find(",OU="));

            fassert(17002, !srvClusterId.empty() && srvClusterId != srvSubjectName);

            // Handle internal cluster member auth, only applies to server-server connections 
            if (srvClusterId == peerClusterId) {
                if (cmdLine.clusterAuthMode.empty() || cmdLine.clusterAuthMode == "keyfile") {
                    return Status(ErrorCodes::AuthenticationFailed, "The provided certificate " 
                                  "can only be used for cluster authentication, not client " 
                                  "authentication. The current configuration does not allow " 
                                  "x.509 cluster authentication, check the --clusterAuthMode flag");
                }
                authorizationSession->grantInternalAuthorization();
            }
            // Handle normal client authentication, only applies to client-server connections
            else {
                authorizationSession->addAndAuthorizeUser(user);
            }
            return Status::OK();
        }
    }
#endif
    CmdAuthenticate cmdAuthenticate;

    class CmdLogout : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        void help(stringstream& h) const { h << "de-authenticate"; }
        virtual LockType locktype() const { return NONE; }
        CmdLogout() : Command("logout") {}
        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationSession* authSession =
                    ClientBasic::getCurrent()->getAuthorizationSession();
            authSession->logoutDatabase(dbname);
            return true;
        }
    } cmdLogout;
}
