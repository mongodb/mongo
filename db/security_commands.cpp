// security_commands.cpp
// security.cpp links with both dbgrid and db.  this file db only -- at least for now.

// security.cpp

#include "stdafx.h"
#include "security.h"
#include "../util/md5.hpp"
#include "json.h" 
#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "commands.h"
#include "jsobj.h"

namespace mongo {

/* authentication

   system.users contains 
     { user : <username>, pwd : <pwd_string>, ... }

   getnonce sends nonce to client

   client then sends { authenticate:1, nonce:<nonce>, user:<username>, key:<key> }

   where <key> is md5(<nonce><username><pwd>) as a BinData type
*/

    boost::thread_specific_ptr<double> lastNonce;

    class CmdGetNonce : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdGetNonce() : Command("getnonce") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            double *d = new double((double) security.getNonce());

            stringstream ss;
            ss << *d;
            result.append("nonce", ss.str() );
            lastNonce.reset(d);
            return true;
        }
    } cmdGetNonce;

    class CmdLogout : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdLogout() : Command("logout") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // database->name is the one we are logging out...
            AuthenticationInfo *ai = authInfo.get();
            assert( ai ); 
            ai->logout(database->name.c_str());
            return true;
        }
    } cmdLogout;

    class CmdAuthenticate : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdAuthenticate() : Command("authenticate") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string user = cmdObj.getStringField("user");
            string key = cmdObj.getStringField("key");
            string nonce = cmdObj.getStringField("nonce");
            
            if( user.empty() || key.empty() || nonce.empty() ) { 
                log() << "field missing/wrong type in received authenticate command " << database->name << '\n';                log() << "field missing/wrong type in received authenticate command " << database->name << '\n';
                errmsg = "auth fails";
                sleepmillis(10);
                return false;
            }
            
            stringstream digestBuilder;

            {
                double *ln = lastNonce.release();
                digestBuilder << *ln;
                
                if( ln == 0 || digestBuilder.str() != nonce ) {
                    log() << "auth: bad nonce received. could be a driver bug or a security attack. db:" << database->name << '\n';                log() << "field missing/wr " << database->name << '\n';
                    errmsg = "auth fails";
                    sleepmillis(30);
                    return false;
                }
            }

            static BSONObj userPattern = fromjson("{\"user\":1}");
            string systemUsers = database->name + ".system.users";
            OCCASIONALLY Helpers::ensureIndex(systemUsers.c_str(), userPattern, "user_1");

            BSONObj userObj;
            {
                BSONObjBuilder b;
                b << "user" << user;
                BSONObj query = b.done();
                if( !Helpers::findOne(systemUsers.c_str(), query, userObj) ) { 
                    log() << "auth: couldn't find user " << user << ", " << systemUsers << '\n';
                    errmsg = "auth fails";
                    return false;
                }
            }
            
            md5digest d;
            {
                
                string pwd = userObj.getStringField("pwd");
                digestBuilder << user << pwd;
                string done = digestBuilder.str();
                
                md5_state_t st;
                md5_init(&st);
                md5_append(&st, (const md5_byte_t *) done.c_str(), done.size());
                md5_finish(&st, d);
            }
            
            string computed = digestToString( d );
            
            if ( key != computed ){
                log() << "auth: key mismatch " << user << ", ns:" << ns << '\n';
                errmsg = "auth fails";
                return false;
            }

            AuthenticationInfo *ai = authInfo.get();
            assert( ai );
            ai->authorize(database->name.c_str());
            return true;
        }
    } cmdAuthenticate;
    
} // namespace mongo
