/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/repl/oplogreader.h"

#include <boost/shared_ptr.hpp>
#include <string>

#include "mongo/base/counter.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rs.h"  // theReplSet
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    //number of readers created;
    //  this happens when the source source changes, a reconfig/network-error or the cursor dies
    static Counter64 readersCreatedStats;
    static ServerStatusMetricField<Counter64> displayReadersCreated(
                                                    "repl.network.readersCreated",
                                                    &readersCreatedStats );


    static const BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    /* Generally replAuthenticate will only be called within system threads to fully authenticate
     * connections to other nodes in the cluster that will be used as part of internal operations.
     * If a user-initiated action results in needing to call replAuthenticate, you can call it
     * with skipAuthCheck set to false. Only do this if you are certain that the proper auth
     * checks have already run to ensure that the user is authorized to do everything that this
     * connection will be used for!
     */
    bool replAuthenticate(DBClientBase *conn, bool skipAuthCheck) {
        if(!getGlobalAuthorizationManager()->isAuthEnabled()) {
            return true;
        }
        if (!skipAuthCheck && !cc().getAuthorizationSession()->hasInternalAuthorization()) {
            log() << "replauthenticate: requires internal authorization, failing" << endl;
            return false;
        }

        string u;
        string p;
        if (internalSecurity.pwd.length() > 0) {
            u = internalSecurity.user;
            p = internalSecurity.pwd;
        }
        else {
            BSONObj user;
            {
                Client::ReadContext ctxt("local.");
                if( !Helpers::findOne("local.system.users", userReplQuery, user) ||
                        // try the first user in local
                        !Helpers::getSingleton("local.system.users", user) ) {
                    log() << "replauthenticate: no user in local.system.users to use for authentication" << endl;
                    return false;
                }
            }
            u = user.getStringField("user");
            p = user.getStringField("pwd");
            massert( 10392 , "bad user object? [1]", !u.empty());
            massert( 10393 , "bad user object? [2]", !p.empty());
        }

        string err;
        if( !conn->auth("local", u.c_str(), p.c_str(), err, false) ) {
            log() << "replauthenticate: can't authenticate to master server, user:" << u << endl;
            return false;
        }

        return true;
    }

    bool replHandshake(DBClientConnection *conn) {
        string myname = getHostName();

        BSONObj me;
        {
            
            Lock::DBWrite l("local");
            // local.me is an identifier for a server for getLastError w:2+
            if ( ! Helpers::getSingleton( "local.me" , me ) ||
                 ! me.hasField("host") ||
                 me["host"].String() != myname ) {

                // clean out local.me
                Helpers::emptyCollection("local.me");

                // repopulate
                BSONObjBuilder b;
                b.appendOID( "_id" , 0 , true );
                b.append( "host", myname );
                me = b.obj();
                Helpers::putSingleton( "local.me" , me );
            }
        }

        BSONObjBuilder cmd;
        cmd.appendAs( me["_id"] , "handshake" );
        if (theReplSet) {
            cmd.append("member", theReplSet->selfId());
            cmd.append("config", theReplSet->myConfig().asBson());
        }

        BSONObj res;
        bool ok = conn->runCommand( "admin" , cmd.obj() , res );
        // ignoring for now on purpose for older versions
        LOG( ok ? 1 : 0 ) << "replHandshake res not: " << ok << " res: " << res << endl;
        return true;
    }

    OplogReader::OplogReader( bool doHandshake ) : 
        _doHandshake( doHandshake ) { 
        
        _tailingQueryOptions = QueryOption_SlaveOk;
        _tailingQueryOptions |= QueryOption_CursorTailable | QueryOption_OplogReplay;
        
        /* TODO: slaveOk maybe shouldn't use? */
        _tailingQueryOptions |= QueryOption_AwaitData;

        readersCreatedStats.increment();
    }

    bool OplogReader::commonConnect(const string& hostName) {
        if( conn() == 0 ) {
            _conn = shared_ptr<DBClientConnection>(new DBClientConnection(false,
                                                                          0,
                                                                          30 /* tcp timeout */));
            string errmsg;
            if ( !_conn->connect(hostName.c_str(), errmsg) ||
                 (getGlobalAuthorizationManager()->isAuthEnabled() &&
                         !replAuthenticate(_conn.get(), true)) ) {
                resetConnection();
                log() << "repl: " << errmsg << endl;
                return false;
            }
        }
        return true;
    }
    
    bool OplogReader::connect(const std::string& hostName) {
        if (conn() != 0) {
            return true;
        }

        if ( ! commonConnect(hostName) ) {
            return false;
        }
        
        
        if ( _doHandshake && ! replHandshake(_conn.get() ) ) {
            return false;
        }

        return true;
    }

    bool OplogReader::connect(const BSONObj& rid, const int from, const string& to) {
        if (conn() != 0) {
            return true;
        }
        if (commonConnect(to)) {
            log() << "handshake between " << from << " and " << to << endl;
            return passthroughHandshake(rid, from);
        }
        return false;
    }

    bool OplogReader::passthroughHandshake(const BSONObj& rid, const int nextOnChainId) {
        BSONObjBuilder cmd;
        cmd.appendAs(rid["_id"], "handshake");
        if (theReplSet) {
            const Member* chainedMember = theReplSet->findById(nextOnChainId);
            if (chainedMember != NULL) {
                cmd.append("config", chainedMember->config().asBson());
            }
        }
        cmd.append("member", nextOnChainId);

        BSONObj res;
        return conn()->runCommand("admin", cmd.obj(), res);
    }

    void OplogReader::tailingQuery(const char *ns, const BSONObj& query, const BSONObj* fields ) {
        verify( !haveCursor() );
        LOG(2) << "repl: " << ns << ".find(" << query.toString() << ')' << endl;
        cursor.reset( _conn->query( ns, query, 0, 0, fields, _tailingQueryOptions ).release() );
    }

    void OplogReader::tailingQueryGTE(const char *ns, OpTime optime, const BSONObj* fields ) {
        BSONObjBuilder gte;
        gte.appendTimestamp("$gte", optime.asDate());
        BSONObjBuilder query;
        query.append("ts", gte.done());
        tailingQuery(ns, query.done(), fields);
    }

}
