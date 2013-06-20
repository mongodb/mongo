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
*/

#include "mongo/db/commands/authentication_commands.h"

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/client/sasl_client_authenticate.h"
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

    bool CmdAuthenticate::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        log() << " authenticate db: " << dbname << " " << cmdObj << endl;

        std::string mechanism = cmdObj.getStringField("mechanism");
        if (mechanism.empty() || mechanism == "MONGODB-CR") {
            return authenticateCR(dbname, cmdObj, errmsg, result);
        }
#ifdef MONGO_SSL
        if (mechanism == "MONGODB-X509") {
            return authenticateX509(dbname, cmdObj, errmsg, result);
        }
#endif
        errmsg = "Unsupported mechanism: " + mechanism;
        result.append(saslCommandCodeFieldName, ErrorCodes::BadValue);
        return false;
    }

    bool CmdAuthenticate::authenticateCR(const string& dbname, 
                                         BSONObj& cmdObj, 
                                         string& errmsg, 
                                         BSONObjBuilder& result) {
        
        string user = cmdObj.getStringField("user");

        if (user == internalSecurity.user && cmdLine.clusterAuthMode == "x509") {
            errmsg = "Mechanism x509 is required for internal cluster authentication";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        if (!_areNonceAuthenticateCommandsEnabled) {
            // SERVER-8461, MONGODB-CR must be enabled for authenticating the internal user, so that
            // cluster members may communicate with each other.
            if (dbname != StringData("local", StringData::LiteralTag()) ||
                user != internalSecurity.user) {
                errmsg = _nonceAuthenticateCommandsDisabledMessage;
                result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
                return false;
            }
        }

        string key = cmdObj.getStringField("key");
        string received_nonce = cmdObj.getStringField("nonce");

        if( user.empty() || key.empty() || received_nonce.empty() ) {
            log() << "field missing/wrong type in received authenticate command "
                  << dbname
                  << endl;
            errmsg = "auth fails";
            sleepmillis(10);
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        stringstream digestBuilder;

        {
            bool reject = false;
            ClientBasic *client = ClientBasic::getCurrent();
            AuthenticationSession *session = client->getAuthenticationSession();
            if (!session || session->getType() != AuthenticationSession::SESSION_TYPE_MONGO) {
                reject = true;
                LOG(1) << "auth: No pending nonce" << endl;
            }
            else {
                nonce64 nonce = static_cast<MongoAuthenticationSession*>(session)->getNonce();
                digestBuilder << hex << nonce;
                reject = digestBuilder.str() != received_nonce;
                if ( reject ) {
                    LOG(1) << "auth: Authentication failed for " << dbname << '$' << user << endl;
                }
            }
            client->resetAuthenticationSession(NULL);

            if ( reject ) {
                log() << "auth: bad nonce received or getnonce not called. could be a driver bug or a security attack. db:" << dbname << endl;
                errmsg = "auth fails";
                sleepmillis(30);
                result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
                return false;
            }
        }

        BSONObj userObj;
        string pwd;
        Status status = getGlobalAuthorizationManager()->getPrivilegeDocument(
                dbname, UserName(user, dbname), &userObj);
        if (!status.isOK()) {
            log() << status.reason() << std::endl;
            errmsg = "auth fails";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }
        pwd = userObj["pwd"].String();

        md5digest d;
        {
            digestBuilder << user << pwd;
            string done = digestBuilder.str();

            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) done.c_str(), done.size());
            md5_finish(&st, d);
        }

        string computed = digestToString( d );

        if ( key != computed ) {
            log() << "auth: key mismatch " << user << ", ns:" << dbname << endl;
            errmsg = "auth fails";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        AuthorizationSession* authorizationSession =
            ClientBasic::getCurrent()->getAuthorizationSession();
        Principal* principal = new Principal(UserName(user, dbname));
        principal->setImplicitPrivilegeAcquisition(true);
        authorizationSession->addAuthorizedPrincipal(principal);

        result.append( "dbname" , dbname );
        result.append( "user" , user );
        return true;
    }

#ifdef MONGO_SSL
    bool CmdAuthenticate::authenticateX509(const string& dbname,
                                           BSONObj& cmdObj, 
                                           string& errmsg, 
                                           BSONObjBuilder& result) {
        if(dbname != "$external") {
            errmsg = "X.509 authentication must always use the $external database.";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        std::string user = cmdObj.getStringField("user");
        ClientBasic *client = ClientBasic::getCurrent();
        AuthorizationSession* authorizationSession = client->getAuthorizationSession();
        StringData subjectName = client->port()->getX509SubjectName();
        
        if (user != subjectName) {
            errmsg = "There is no x.509 client certificate matching the user.";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }
        else {
            StringData srvSubjectName = getSSLManager()->getServerSubjectName();
            StringData srvClusterId = srvSubjectName.substr(0, srvSubjectName.find("/CN")+1);
            StringData peerClusterId = subjectName.substr(0, subjectName.find("/CN")+1);

            // Handle internal cluster member auth, only applies to server-server connections 
            if (srvClusterId == peerClusterId) {
                if (cmdLine.clusterAuthMode == "keyfile") {
                    errmsg = "X509 authentication is not allowed for cluster authentication";
                    result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
                    return false;
                }
                authorizationSession->grantInternalAuthorization(UserName(user, "$external"));
            }
            // Handle normal client authentication, only applies to client-server connections
            else {
                Principal* principal = new Principal(UserName(user, "$external"));
                principal->setImplicitPrivilegeAcquisition(true);
                authorizationSession->addAuthorizedPrincipal(principal);
            }
            result.append( "dbname" , dbname );
            result.append( "user" , user );
            return true;
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
