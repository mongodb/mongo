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

#include "mongo/pch.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/matcher.h"
#include "mongo/s/client_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/shard.h"
#include "mongo/util/concurrency/thread_name.h"

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

    Client::Client(const string& desc, AbstractMessagingPort *p) :
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

        verify( currentClient.get() == 0 );

        string fullDesc = desc;
        if ( str::equals( "conn" , desc ) && mp != NULL )
            fullDesc = str::stream() << desc << mp->connectionId();

        setThreadName( fullDesc.c_str() );

        Client *c = new Client( fullDesc, mp );
        currentClient.reset(c);
        mongo::lastError.initThread();
        c->setAuthorizationSession(new AuthorizationSession(new AuthzSessionExternalStateMongos(
                getGlobalAuthorizationManager())));
        return *c;
    }

    /* resets the client for the current thread */
    // Needed here since we may want to use for testing when linked against mongos
    void Client::resetThread( const StringData& origThreadName ) {
        verify( currentClient.get() != 0 );

        // Detach all client info from thread
        mongo::lastError.reset(NULL);
        currentClient.get()->shutdown();
        currentClient.reset(NULL);

        setThreadName( origThreadName.rawData() );
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
        std::string dbname = nsToDatabase(ns);

        Status status = _checkAuthorization(c, &client, dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            appendCommandStatus(result, status);
            return;
        }

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
