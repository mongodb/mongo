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

#include "mongo/db/repl/replication_server_status.h"

#include <list>
#include <vector>
#include <boost/scoped_ptr.hpp>

#include "mongo/client/connpool.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/replutil.h"

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

    OpCounterServerStatusSection replOpCounterServerStatusSection( "opcountersRepl", &replOpCounters );

}
