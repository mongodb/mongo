// s_only.cpp

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/pch.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/s/client_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/shard.h"
#include "mongo/util/log.h"
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
        ClientInfo::get()->addShardHost( addr );
    }

    TSP_DEFINE(Client,currentClient)

    Client::Client(const string& desc, AbstractMessagingPort *p) :
        ClientBasic(p),
        _desc(desc),
        _connectionId(),
        _god(0),
        _lastOp(0),
        _shutdown(false) {
    }
    Client::~Client() {}
    bool Client::shutdown() { return true; }

    void Client::initThread(const char *desc, AbstractMessagingPort *mp) {
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
    }

    string Client::clientAddress(bool includePort) const {
        ClientInfo * ci = ClientInfo::get();
        if ( ci )
            return ci->getRemote().toString();
        return "";
    }

    // Need a version that takes a Client to match the mongod interface so the web server can call
    // execCommand and not need to worry if it's in a mongod or mongos.
    void Command::execCommand(OperationContext* txn,
                              Command * c,
                              int queryOptions,
                              const char *ns,
                              BSONObj& cmdObj,
                              BSONObjBuilder& result,
                              bool fromRepl ) {
        execCommandClientBasic(txn, c, *txn->getClient(), queryOptions, ns, cmdObj, result, fromRepl);
    }

    void Command::execCommandClientBasic(OperationContext* txn,
                                         Command * c ,
                                         ClientBasic& client,
                                         int queryOptions,
                                         const char *ns,
                                         BSONObj& cmdObj,
                                         BSONObjBuilder& result,
                                         bool fromRepl ) {
        std::string dbname = nsToDatabase(ns);

        if (cmdObj.getBoolField("help")) {
            stringstream help;
            help << "help for: " << c->name << " ";
            c->help( help );
            result.append( "help" , help.str() );
            result.append("lockType", c->isWriteCommandForConfigServer() ? 1 : 0);
            appendCommandStatus(result, true, "");
            return;
        }

        Status status = _checkAuthorization(c, &client, dbname, cmdObj, fromRepl);
        if (!status.isOK()) {
            appendCommandStatus(result, status);
            return;
        }

        c->_commandsExecuted.increment();

        std::string errmsg;
        bool ok;
        try {
            ok = c->run( txn, dbname , cmdObj, queryOptions, errmsg, result, false );
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

        if ( !ok ) {
            c->_commandsFailed.increment();
        }

        appendCommandStatus(result, ok, errmsg);
    }
}
