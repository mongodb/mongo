/*
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

#include "mongo/pch.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/mongo_authentication_session.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_set.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/random.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/mongoutils/str.h"

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
        CmdGetNonce() : Command("getnonce") {
            _random = SecureRandom::create();
        }
        
        virtual bool requiresAuth() { return false; }
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
            if (!_areNonceAuthenticateCommandsEnabled) {
                errmsg = _nonceAuthenticateCommandsDisabledMessage;
                return false;
            }

            nonce64 n = _random->nextInt64();
            stringstream ss;
            ss << hex << n;
            result.append("nonce", ss.str() );
            ClientBasic::getCurrent()->resetAuthenticationSession(
                    new MongoAuthenticationSession(n));
            return true;
        }

        SecureRandom* _random;
    } cmdGetNonce;

    CmdLogout cmdLogout;

    Status authenticateAndAuthorizePrincipal(const std::string& principalName,
                                             const std::string& dbname,
                                             const BSONObj& userObj) {
        AuthorizationManager* authorizationManager =
                ClientBasic::getCurrent()->getAuthorizationManager();
        Principal* principal = new Principal(PrincipalName(principalName, dbname));
        authorizationManager->addAuthorizedPrincipal(principal);
        return authorizationManager->acquirePrivilegesFromPrivilegeDocument(dbname,
                                                                            principal->getName(),
                                                                            userObj);
    }

    bool CmdAuthenticate::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        if (!_areNonceAuthenticateCommandsEnabled) {
            errmsg = _nonceAuthenticateCommandsDisabledMessage;
            return false;
        }

        log() << " authenticate db: " << dbname << " " << cmdObj << endl;

        string user = cmdObj.getStringField("user");
        string key = cmdObj.getStringField("key");
        string received_nonce = cmdObj.getStringField("nonce");

        if( user.empty() || key.empty() || received_nonce.empty() ) {
            log() << "field missing/wrong type in received authenticate command "
                  << dbname
                  << endl;
            errmsg = "auth fails";
            sleepmillis(10);
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
                return false;
            }
        }

        BSONObj userObj;
        string pwd;
        Status status = ClientBasic::getCurrent()->getAuthorizationManager()->getPrivilegeDocument(
                dbname, PrincipalName(user, dbname), &userObj);
        if (!status.isOK()) {
            log() << status.reason() << std::endl;
            errmsg = status.reason();
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
            return false;
        }

        status = authenticateAndAuthorizePrincipal(user, dbname, userObj);
        uassert(16500,
                mongoutils::str::stream() << "Problem acquiring privileges for principal \""
                        << user << "\": " << status.reason(),
                status == Status::OK());

        bool readOnly = userObj["readOnly"].trueValue();
        // TODO: remove this once all auth checking goes through AuthorizationManager instead of
        // AuthenticationInfo
        authenticate(dbname, user, readOnly );
        
        
        result.append( "dbname" , dbname );
        result.append( "user" , user );
        result.appendBool( "readOnly" , readOnly );
        

        return true;
    }

    CmdAuthenticate cmdAuthenticate;

} // namespace mongo
