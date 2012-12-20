// s_only.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/auth_external_state_s.h"
#include "mongo/s/shard.h"
#include "mongo/s/grid.h"
#include "request.h"
#include "client_info.h"
#include "../db/dbhelpers.h"
#include "../db/matcher.h"
#include "../db/commands.h"

/*
  most a pile of hacks to make linking nicer

 */
namespace mongo {

    void* remapPrivateView(void *oldPrivateAddr) {
        log() << "remapPrivateView called in mongos, aborting" << endl;
        fassertFailed(16462);
    }

    /** When this callback is run, we record a shard that we've used for useful work
     *  in an operation to be read later by getLastError()
    */
    void usingAShardConnection( const string& addr ) {
        ClientInfo::get()->addShard( addr );
    }

    TSP_DEFINE(Client,currentClient)

    LockState::LockState(){} // ugh

    Client::Client(const char *desc , AbstractMessagingPort *p) :
        ClientBasic(p),
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0) {
    }
    Client::~Client() {}
    bool Client::shutdown() { return true; }

    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        // mp is non-null only for client connections, and mongos uses ClientInfo for those
        massert(16478, "Client being used for incoming connection thread in mongos", mp == NULL);
        setThreadName(desc);
        verify( currentClient.get() == 0 );
        // mp is always NULL in mongos. Threads for client connections use ClientInfo in mongos
        massert(16482,
                "Non-null messaging port provided to Client::initThread in a mongos",
                mp == NULL);
        Client *c = new Client(desc, mp);
        c->setAuthorizationManager(new AuthorizationManager(new AuthExternalStateMongos()));
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    string Client::clientAddress(bool includePort) const {
        ClientInfo * ci = ClientInfo::get();
        if ( ci )
            return ci->getRemote();
        return "";
    }

    // Need a version that takes a Client to match the mongod interface so the web server can call
    // execCommand and not need to worry if it's in a mongod or mongos.
    void Command::execCommand(Command * c,
                              Client& client,
                              int queryOptions,
                              const char *ns,
                              BSONObj& cmdObj,
                              BSONObjBuilder& result,
                              bool fromRepl ) {
        execCommandClientBasic(c, client, queryOptions, ns, cmdObj, result, fromRepl);
    }

    void Command::execCommandClientBasic(Command * c ,
                                         ClientBasic& client,
                                         int queryOptions,
                                         const char *ns,
                                         BSONObj& cmdObj,
                                         BSONObjBuilder& result,
                                         bool fromRepl ) {
        verify(c);

        std::string dbname = nsToDatabase(ns);

        // Access control checks
        if (!noauth) {
            std::vector<Privilege> privileges;
            c->addRequiredPrivileges(dbname, cmdObj, &privileges);
            AuthorizationManager* authManager = client.getAuthorizationManager();
            if (c->requiresAuth() && (!authManager->checkAuthForPrivileges(privileges).isOK())) {
                result.append("note", str::stream() << "not authorized for command: " <<
                                    c->name << " on database " << dbname);
                appendCommandStatus(result, false, "unauthorized");
                return;
            }
        }
        if (c->adminOnly() && c->localHostOnlyIfNoAuth(cmdObj) && noauth &&
                !client.getIsLocalHostConnection()) {
            log() << "command denied: " << cmdObj.toString() << endl;
            appendCommandStatus(result,
                               false,
                               "unauthorized: this command must run from localhost when running db "
                               "without auth");
            return;
        }
        if (c->adminOnly() && !startsWith(ns, "admin.")) {
            log() << "command denied: " << cmdObj.toString() << endl;
            appendCommandStatus(result, false, "access denied - use admin db");
            return;
        }
        // End of access control checks

        if (cmdObj.getBoolField("help")) {
            stringstream help;
            help << "help for: " << c->name << " ";
            c->help( help );
            result.append( "help" , help.str() );
            result.append( "lockType" , c->locktype() );
            appendCommandStatus(result, true, "");
            return;
        }
        std::string errmsg;
        bool ok;
        try {
            ok = c->run( dbname , cmdObj, queryOptions, errmsg, result, false );
        }
        catch (DBException& e) {
            ok = false;
            int code = e.getCode();
            if (code == RecvStaleConfigCode) { // code for StaleConfigException
                throw;
            }

            stringstream ss;
            ss << "exception: " << e.what();
            errmsg = ss.str();
            result.append( "code" , code );
        }

        appendCommandStatus(result, ok, errmsg);
    }
}
