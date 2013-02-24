// @file repl.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"

#include <boost/thread/thread.hpp>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "repl.h"
#include "../util/net/message.h"
#include "../util/background.h"
#include "../client/connpool.h"
#include "pdfile.h"
#include "db.h"
#include "commands.h"
#include "cmdline.h"
#include "repl_block.h"
#include "repl/rs.h"
#include "replutil.h"
#include "repl/connections.h"
#include "ops/update.h"
#include "pcrecpp.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/instance.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/queryutil.h"
#include "mongo/base/counter.h"
#include "mongo/db/oplog.h"
#include "mongo/db/repl/master_slave.h"

namespace mongo {

    // our config from command line etc.
    ReplSettings replSettings;

    bool anyReplEnabled() {
        return replSettings.slave || replSettings.master || theReplSet;
    }

    bool replAuthenticate(DBClientBase *conn, bool skipAuthCheck);

    void appendReplicationInfo(BSONObjBuilder& result, int level) {
        if ( replSet ) {
            if( theReplSet == 0 || theReplSet->state().shunned() ) {
                result.append("ismaster", false);
                result.append("secondary", false);
                result.append("info", ReplSet::startupStatusMsg.get());
                result.append( "isreplicaset" , true );
            }
            else {
                theReplSet->fillIsMaster(result);
            }
            return;
        }
        
        if ( replAllDead ) {
            result.append("ismaster", 0);
            string s = string("dead: ") + replAllDead;
            result.append("info", s);
        }
        else {
            result.appendBool("ismaster", _isMaster() );
        }
        
        if ( level && replSet ) {
            result.append( "info" , "is replica set" );
        }
        else if ( level ) {
            BSONObjBuilder sources( result.subarrayStart( "sources" ) );
            
            int n = 0;
            list<BSONObj> src;
            {
                Client::ReadContext ctx("local.sources", dbpath);
                shared_ptr<Cursor> c = findTableScan("local.sources", BSONObj());
                while ( c->ok() ) {
                    src.push_back(c->current());
                    c->advance();
                }
            }
            
            for( list<BSONObj>::const_iterator i = src.begin(); i != src.end(); i++ ) {
                BSONObj s = *i;
                BSONObjBuilder bb;
                bb.append( s["host"] );
                string sourcename = s["source"].valuestr();
                if ( sourcename != "main" )
                    bb.append( s["source"] );
                {
                    BSONElement e = s["syncedTo"];
                    BSONObjBuilder t( bb.subobjStart( "syncedTo" ) );
                    t.appendDate( "time" , e.timestampTime() );
                    t.append( "inc" , e.timestampInc() );
                    t.done();
                }
                
                if ( level > 1 ) {
                    wassert( !Lock::isLocked() );
                    // note: there is no so-style timeout on this connection; perhaps we should have one.
                    scoped_ptr<ScopedDbConnection> conn( ScopedDbConnection::getInternalScopedDbConnection( s["host"].valuestr() ) );
                    
                    DBClientConnection *cliConn = dynamic_cast< DBClientConnection* >( &conn->conn() );
                    if ( cliConn && replAuthenticate(cliConn, false) ) {
                        BSONObj first = conn->get()->findOne( (string)"local.oplog.$" + sourcename,
                                                              Query().sort( BSON( "$natural" << 1 ) ) );
                        BSONObj last = conn->get()->findOne( (string)"local.oplog.$" + sourcename,
                                                             Query().sort( BSON( "$natural" << -1 ) ) );
                        bb.appendDate( "masterFirst" , first["ts"].timestampTime() );
                        bb.appendDate( "masterLast" , last["ts"].timestampTime() );
                        double lag = (double) (last["ts"].timestampTime() - s["syncedTo"].timestampTime());
                        bb.append( "lagSeconds" , lag / 1000 );
                    }
                    conn->done();
                }
                
                sources.append( BSONObjBuilder::numStr( n++ ) , bb.obj() );
            }
            
            sources.done();
        }
    }
    
    class ReplicationInfoServerStatus : public ServerStatusSection {
    public:
        ReplicationInfoServerStatus() : ServerStatusSection( "repl" ){}
        bool includeByDefault() const { return true; }
        
        BSONObj generateSection(const BSONElement& configElement) const {
            if ( ! anyReplEnabled() )
                return BSONObj();
            
            int level = configElement.numberInt();
            
            BSONObjBuilder result;
            appendReplicationInfo(result, level);
            return result.obj();
        }
    } replicationInfoServerStatus;

    class CmdIsMaster : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void help( stringstream &help ) const {
            help << "Check if this server is primary for a replica pair/set; also if it is --master or --slave in simple master/slave setups.\n";
            help << "{ isMaster : 1 }";
        }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        CmdIsMaster() : Command("isMaster", true, "ismaster") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
               authenticated.
            */
            appendReplicationInfo(result, 0);

            result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
            result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
            result.appendDate("localTime", jsTime());
            return true;
        }
    } cmdismaster;


    static const BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    /* Generally replAuthenticate will only be called within system threads to fully authenticate
     * connections to other nodes in the cluster that will be used as part of internal operations.
     * If a user-initiated action results in needing to call replAuthenticate, you can call it
     * with skipAuthCheck set to false. Only do this if you are certain that the proper auth
     * checks have already run to ensure that the user is authorized to do everything that this
     * connection will be used for!
     */
    bool replAuthenticate(DBClientBase *conn, bool skipAuthCheck) {
        if( noauth ) {
            return true;
        }
        if (!skipAuthCheck && !cc().getAuthorizationManager()->hasInternalAuthorization()) {
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

    //number of readers created;
    //  this happens when the source source changes, a reconfig/network-error or the cursor dies
    static Counter64 readersCreatedStats;
    static ServerStatusMetricField<Counter64> displayReadersCreated(
                                                    "repl.network.readersCreated",
                                                    &readersCreatedStats );

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
                 (!noauth && !replAuthenticate(_conn.get(), true)) ) {
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


    void startReplSets(ReplSetCmdline*);
    void startReplication() {
        /* if we are going to be a replica set, we aren't doing other forms of replication. */
        if( !cmdLine._replSet.empty() ) {
            if( replSettings.slave || replSettings.master ) {
                log() << "***" << endl;
                log() << "ERROR: can't use --slave or --master replication options with --replSet" << endl;
                log() << "***" << endl;
            }
            newRepl();

            replSet = true;
            ReplSetCmdline *replSetCmdline = new ReplSetCmdline(cmdLine._replSet);
            boost::thread t( boost::bind( &startReplSets, replSetCmdline) );

            return;
        }

        startMasterSlave();
    }

    /** we allow queries to SimpleSlave's */
    void replVerifyReadsOk(const ParsedQuery* pq) {
        if( replSet ) {
            // todo: speed up the secondary case.  as written here there are 2 mutex entries, it
            // can b 1.
            if( isMaster() ) return;
            uassert(13435, "not master and slaveOk=false",
                    !pq || pq->hasOption(QueryOption_SlaveOk) || pq->hasReadPref());
            uassert(13436,
                    "not master or secondary; cannot currently read from this replSet member",
                    theReplSet && theReplSet->isSecondary() );
        }
        else {
            uassert( 10107,
                     "not master", 
                     isMaster() || 
                     (!pq || pq->hasOption(QueryOption_SlaveOk)) ||
                     replSettings.slave == SimpleSlave );
        }
    }

    OpCounterServerStatusSection replOpCounterServerStatusSection( "opcountersRepl", &replOpCounters );

} // namespace mongo
