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
#include "request.h"
#include "client.h"
#include "../db/dbhelpers.h"
#include "../db/matcher.h"
#include "../db/commands.h"

/*
  most a pile of hacks to make linking nicer

 */
namespace mongo {

    TSP_DEFINE(Client,currentClient)

    LockState::LockState(){} // ugh

    Client::Client(const char *desc , AbstractMessagingPort *p) :
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0),
        _mp(p) {
    }
    Client::~Client() {}
    bool Client::shutdown() { return true; }

    static unsigned long long nThreads = 0;
    void assertStartingUp() { 
        dassert( nThreads <= 1 );
    }
    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        DEV nThreads++; // never decremented.  this is for casi class asserts
        setThreadName(desc);
        verify( currentClient.get() == 0 );
        Client *c = new Client(desc, mp);
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

    bool execCommand( Command * c ,
                      Client& client , int queryOptions ,
                      const char *ns, BSONObj& cmdObj ,
                      BSONObjBuilder& result,
                      bool fromRepl ) {
        verify(c);

        string dbname = nsToDatabase( ns );

        if ( cmdObj["help"].trueValue() ) {
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append( "lockType" , c->locktype() );
            return true;
        }

        if ( c->adminOnly() ) {
            if ( dbname != "admin" ) {
                result.append( "errmsg" ,  "access denied- use admin db" );
                log() << "command denied: " << cmdObj.toString() << endl;
                return false;
            }
            log( 2 ) << "command: " << cmdObj << endl;
        }

        if (!client.getAuthenticationInfo()->isAuthorized(dbname)) {
            result.append("errmsg" , "unauthorized");
            result.append("note" , "from execCommand" );
            return false;
        }

        string errmsg;
        int ok = c->run( dbname , cmdObj , queryOptions, errmsg , result , fromRepl );
        if ( ! ok )
            result.append( "errmsg" , errmsg );
        return ok;
    }
}
