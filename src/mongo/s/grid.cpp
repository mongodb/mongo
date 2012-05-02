// grid.cpp

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

#include "pch.h"

#include <iomanip>
#include "../client/connpool.h"
#include "../util/stringutils.h"
#include "../util/startup_test.h"
#include "../db/namespacestring.h"
#include "mongo/db/json.h"

#include "grid.h"
#include "shard.h"
#include "pcrecpp.h"

namespace mongo {

    DBConfigPtr Grid::getDBConfig( string database , bool create , const string& shardNameHint ) {
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }

        if ( database == "config" )
            return configServerPtr;

        uassert( 15918 , str::stream() << "invalid database name: " << database , NamespaceString::validDBName( database ) );

        scoped_lock l( _lock );

        DBConfigPtr& cc = _databases[database];
        if ( !cc ) {
            cc.reset(new DBConfig( database ));
            if ( ! cc->load() ) {
                if ( create ) {
                    // note here that cc->primary == 0.
                    log() << "couldn't find database [" << database << "] in config db" << endl;

                    {
                        // lets check case
                        ScopedDbConnection conn( configServer.modelServer() );
                        BSONObjBuilder b;
                        b.appendRegex( "_id" , (string)"^" +
                                       pcrecpp::RE::QuoteMeta( database ) + "$" , "i" );
                        BSONObj d = conn->findOne( ShardNS::database , b.obj() );
                        conn.done();

                        if ( ! d.isEmpty() ) {
                            cc.reset();
                            stringstream ss;
                            ss <<  "can't have 2 databases that just differ on case "
                               << " have: " << d["_id"].String()
                               << " want to add: " << database;

                            uasserted( DatabaseDifferCaseCode ,ss.str() );
                        }
                    }

                    Shard primary;
                    if ( database == "admin" ) {
                        primary = configServer.getPrimary();

                    }
                    else if ( shardNameHint.empty() ) {
                        primary = Shard::pick();

                    }
                    else {
                        // use the shard name if provided
                        Shard shard;
                        shard.reset( shardNameHint );
                        primary = shard;
                    }

                    if ( primary.ok() ) {
                        cc->setPrimary( primary.getName() ); // saves 'cc' to configDB
                        log() << "\t put [" << database << "] on: " << primary << endl;
                    }
                    else {
                        cc.reset();
                        log() << "\t can't find a shard to put new db on" << endl;
                        uasserted( 10185 ,  "can't find a shard to put new db on" );
                    }
                }
                else {
                    cc.reset();
                }
            }

        }

        return cc;
    }

    void Grid::removeDB( string database ) {
        uassert( 10186 ,  "removeDB expects db name" , database.find( '.' ) == string::npos );
        scoped_lock l( _lock );
        _databases.erase( database );

    }

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ) {
        _allowLocalShard = allow;
    }

    bool Grid::addShard( string* name , const ConnectionString& servers , long long maxSize , string& errMsg ) {
        // name can be NULL, so provide a dummy one here to avoid testing it elsewhere
        string nameInternal;
        if ( ! name ) {
            name = &nameInternal;
        }

        ReplicaSetMonitorPtr rsMonitor;

        // Check whether the host (or set) exists and run several sanity checks on this request.
        // There are two set of sanity checks: making sure adding this particular shard is consistent
        // with the replica set state (if it exists) and making sure this shards databases can be
        // brought into the grid without conflict.

        vector<string> dbNames;
        try {
            ScopedDbConnection newShardConn( servers );
            newShardConn->getLastError();

            if ( newShardConn->type() == ConnectionString::SYNC ) {
                newShardConn.done();
                errMsg = "can't use sync cluster as a shard.  for replica set, have to use <setname>/<server1>,<server2>,...";
                return false;
            }
            
            BSONObj resIsMongos;
            bool ok = newShardConn->runCommand( "admin" , BSON( "isdbgrid" << 1 ) , resIsMongos );

            // should return ok=0, cmd not found if it's a normal mongod
            if ( ok ) {
                errMsg = "can't add a mongos process as a shard";
                newShardConn.done();
                return false;
            }

            BSONObj resIsMaster;
            ok =  newShardConn->runCommand( "admin" , BSON( "isMaster" << 1 ) , resIsMaster );
            if ( !ok ) {
                ostringstream ss;
                ss << "failed running isMaster: " << resIsMaster;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard has only one host, make sure it is not part of a replica set
            string setName = resIsMaster["setName"].str();
            string commandSetName = servers.getSetName();
            if ( commandSetName.empty() && ! setName.empty() ) {
                ostringstream ss;
                ss << "host is part of set: " << setName << " use replica set url format <setname>/<server1>,<server2>,....";
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }
            if ( !commandSetName.empty() && setName.empty() ) {
                ostringstream ss;
                ss << "host did not return a set name, is the replica set still initializing? " << resIsMaster;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard is part of replica set, make sure it is the right one
            if ( ! commandSetName.empty() && ( commandSetName != setName ) ) {
                ostringstream ss;
                ss << "host is part of a different set: " << setName;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard is part of a replica set, make sure all the hosts mentioned in 'servers' are part of
            // the set. It is fine if not all members of the set are present in 'servers'.
            bool foundAll = true;
            string offendingHost;
            if ( ! commandSetName.empty() ) {
                set<string> hostSet;
                BSONObjIterator iter( resIsMaster["hosts"].Obj() );
                while ( iter.more() ) {
                    hostSet.insert( iter.next().String() ); // host:port
                }
                if ( resIsMaster["passives"].isABSONObj() ) {
                    BSONObjIterator piter( resIsMaster["passives"].Obj() );
                    while ( piter.more() ) {
                        hostSet.insert( piter.next().String() ); // host:port
                    }
                }
                if ( resIsMaster["arbiters"].isABSONObj() ) {
                    BSONObjIterator piter( resIsMaster["arbiters"].Obj() );
                    while ( piter.more() ) {
                        hostSet.insert( piter.next().String() ); // host:port
                    }
                }

                vector<HostAndPort> hosts = servers.getServers();
                for ( size_t i = 0 ; i < hosts.size() ; i++ ) {
                    if (!hosts[i].hasPort()) {
                        hosts[i].setPort(CmdLine::DefaultDBPort);
                    }
                    string host = hosts[i].toString(); // host:port
                    if ( hostSet.find( host ) == hostSet.end() ) {
                        offendingHost = host;
                        foundAll = false;
                        break;
                    }
                }
            }
            if ( ! foundAll ) {
                ostringstream ss;
                ss << "in seed list " << servers.toString() << ", host " << offendingHost
                   << " does not belong to replica set " << setName;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // shard name defaults to the name of the replica set
            if ( name->empty() && ! setName.empty() )
                *name = setName;

            // In order to be accepted as a new shard, that mongod must not have any database name that exists already
            // in any other shards. If that test passes, the new shard's databases are going to be entered as
            // non-sharded db's whose primary is the newly added shard.

            BSONObj resListDB;
            ok = newShardConn->runCommand( "admin" , BSON( "listDatabases" << 1 ) , resListDB );
            if ( !ok ) {
                ostringstream ss;
                ss << "failed listing " << servers.toString() << "'s databases:" << resListDB;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            BSONObjIterator i( resListDB["databases"].Obj() );
            while ( i.more() ) {
                BSONObj dbEntry = i.next().Obj();
                const string& dbName = dbEntry["name"].String();
                if ( _isSpecialLocalDB( dbName ) ) {
                    // 'local', 'admin', and 'config' are system DBs and should be excluded here
                    continue;
                }
                else {
                    dbNames.push_back( dbName );
                }
            }

            if ( newShardConn->type() == ConnectionString::SET ) 
                rsMonitor = ReplicaSetMonitor::get( setName );

            newShardConn.done();
        }
        catch ( DBException& e ) {
            if ( servers.type() == ConnectionString::SET ) {
                ReplicaSetMonitor::remove( servers.getSetName() );
            }
            ostringstream ss;
            ss << "couldn't connect to new shard ";
            ss << e.what();
            errMsg = ss.str();
            return false;
        }

        // check that none of the existing shard candidate's db's exist elsewhere
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ) {
            DBConfigPtr config = getDBConfig( *it , false );
            if ( config.get() != NULL ) {
                ostringstream ss;
                ss << "can't add shard " << servers.toString() << " because a local database '" << *it;
                ss << "' exists in another " << config->getPrimary().toString();
                errMsg = ss.str();
                return false;
            }
        }

        // if a name for a shard wasn't provided, pick one.
        if ( name->empty() && ! _getNewShardName( name ) ) {
            errMsg = "error generating new shard name";
            return false;
        }

        // build the ConfigDB shard document
        BSONObjBuilder b;
        b.append( "_id" , *name );
        b.append( "host" , rsMonitor ? rsMonitor->getServerAddress() : servers.toString() );
        if ( maxSize > 0 ) {
            b.append( ShardFields::maxSize.name() , maxSize );
        }
        BSONObj shardDoc = b.obj();

        {
            ScopedDbConnection conn( configServer.getPrimary() );

            // check whether the set of hosts (or single host) is not an already a known shard
            BSONObj old = conn->findOne( ShardNS::shard , BSON( "host" << servers.toString() ) );
            if ( ! old.isEmpty() ) {
                errMsg = "host already used";
                conn.done();
                return false;
            }

            log() << "going to add shard: " << shardDoc << endl;

            conn->insert( ShardNS::shard , shardDoc );
            errMsg = conn->getLastError();
            if ( ! errMsg.empty() ) {
                log() << "error adding shard: " << shardDoc << " err: " << errMsg << endl;
                conn.done();
                return false;
            }

            conn.done();
        }

        Shard::reloadShardInfo();

        // add all databases of the new shard
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ) {
            DBConfigPtr config = getDBConfig( *it , true , *name );
            if ( ! config ) {
                log() << "adding shard " << servers << " even though could not add database " << *it << endl;
            }
        }

        return true;
    }

    bool Grid::knowAboutShard( const string& name ) const {
        ScopedDbConnection conn( configServer.getPrimary()  );
        BSONObj shard = conn->findOne( ShardNS::shard , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    bool Grid::_getNewShardName( string* name ) const {
        DEV verify( name );

        bool ok = false;
        int count = 0;

        ScopedDbConnection conn( configServer.getPrimary() );
        BSONObj o = conn->findOne( ShardNS::shard , Query( fromjson ( "{_id: /^shard/}" ) ).sort(  BSON( "_id" << -1 ) ) );
        if ( ! o.isEmpty() ) {
            string last = o["_id"].String();
            istringstream is( last.substr( 5 ) );
            is >> count;
            count++;
        }
        if (count < 9999) {
            stringstream ss;
            ss << "shard" << setfill('0') << setw(4) << count;
            *name = ss.str();
            ok = true;
        }
        conn.done();

        return ok;
    }

    /*
     * Returns whether balancing is enabled, with optional namespace "ns" parameter for balancing on a particular
     * collection.
     */
    bool Grid::shouldBalance( const string& ns ) const {

        ScopedDbConnection conn( configServer.getPrimary() );
        BSONObj balancerDoc;
        BSONObj collDoc;

        try {
            // look for the stop balancer marker
            balancerDoc = conn->findOne( ShardNS::settings, BSON( "_id" << "balancer" ) );
            if( ns.size() > 0 ) collDoc = conn->findOne( ShardNS::collection, BSON( "_id" << ns ) );
            conn.done();
        }
        catch( DBException& e ){
            conn.kill();
            warning() << "could not determine whether balancer should be running, error getting config data from " << conn.getHost() << causedBy( e ) << endl;
            // if anything goes wrong, we shouldn't try balancing
            return false;
        }

        boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
        if ( _balancerStopped( balancerDoc ) || ! _inBalancingWindow( balancerDoc , now ) ) {
            return false;
        }

        if( collDoc["noBalance"].trueValue() ) return false;
        return true;
    }

    bool Grid::_balancerStopped( const BSONObj& balancerDoc ) {
        // check the 'stopped' marker maker
        // if present, it is a simple bool
        BSONElement stoppedElem = balancerDoc["stopped"];
        return stoppedElem.trueValue();
    }

    bool Grid::_inBalancingWindow( const BSONObj& balancerDoc , const boost::posix_time::ptime& now ) {
        // check the 'activeWindow' marker
        // if present, it is an interval during the day when the balancer should be active
        // { start: "08:00" , stop: "19:30" }, strftime format is %H:%M
        BSONElement windowElem = balancerDoc["activeWindow"];
        if ( windowElem.eoo() ) {
            return true;
        }

        // check if both 'start' and 'stop' are present
        if ( ! windowElem.isABSONObj() ) {
            warning() << "'activeWindow' format is { start: \"hh:mm\" , stop: ... }" << balancerDoc << endl;
            return true;
        }
        BSONObj intervalDoc = windowElem.Obj();
        const string start = intervalDoc["start"].str();
        const string stop = intervalDoc["stop"].str();
        if ( start.empty() || stop.empty() ) {
            warning() << "must specify both start and end of balancing window: " << intervalDoc << endl;
            return true;
        }

        // check that both 'start' and 'stop' are valid time-of-day
        boost::posix_time::ptime startTime, stopTime;
        if ( ! toPointInTime( start , &startTime ) || ! toPointInTime( stop , &stopTime ) ) {
            warning() << "cannot parse active window (use hh:mm 24hs format): " << intervalDoc << endl;
            return true;
        }

        if ( logLevel ) {
            stringstream ss;
            ss << " now: " << now
               << " startTime: " << startTime 
               << " stopTime: " << stopTime;
            log() << "_inBalancingWindow: " << ss.str() << endl;
        }

        // allow balancing if during the activeWindow
        // note that a window may be open during the night
        if ( stopTime > startTime ) {
            if ( ( now >= startTime ) && ( now <= stopTime ) ) {
                return true;
            }
        }
        else if ( startTime > stopTime ) {
            if ( ( now >=startTime ) || ( now <= stopTime ) ) {
                return true;
            }
        }

        return false;
    }

    unsigned long long Grid::getNextOpTime() const {
        ScopedDbConnection conn( configServer.getPrimary() );

        BSONObj result;
        massert( 10421 ,  "getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime" ) );
        conn.done();

        return result["optime"]._numberLong();
    }

    bool Grid::_isSpecialLocalDB( const string& dbName ) {
        return ( dbName == "local" ) || ( dbName == "admin" ) || ( dbName == "config" );
    }

    void Grid::flushConfig() {
        scoped_lock lk( _lock );
        _databases.clear();
    }

    BSONObj Grid::getConfigSetting( string name ) const {
        ScopedDbConnection conn( configServer.getPrimary() );
        BSONObj result = conn->findOne( ShardNS::settings, BSON( "_id" << name ) );
        conn.done();

        return result;
    }

    Grid grid;


    // unit tests

    class BalancingWindowUnitTest : public StartupTest {
    public:
        void run() {
            
            if ( ! cmdLine.isMongos() )
                return;

            // T0 < T1 < now < T2 < T3 and Error
            const string T0 = "9:00";
            const string T1 = "11:00";
            boost::posix_time::ptime now( currentDate(), boost::posix_time::hours( 13 ) + boost::posix_time::minutes( 48 ) );
            const string T2 = "17:00";
            const string T3 = "21:30";
            const string E = "28:35";

            BSONObj w1 = BSON( "activeWindow" << BSON( "start" << T0 << "stop" << T1 ) ); // closed in the past
            BSONObj w2 = BSON( "activeWindow" << BSON( "start" << T2 << "stop" << T3 ) ); // not opened until the future
            BSONObj w3 = BSON( "activeWindow" << BSON( "start" << T1 << "stop" << T2 ) ); // open now
            BSONObj w4 = BSON( "activeWindow" << BSON( "start" << T3 << "stop" << T2 ) ); // open since last day

            verify( ! Grid::_inBalancingWindow( w1 , now ) );
            verify( ! Grid::_inBalancingWindow( w2 , now ) );
            verify( Grid::_inBalancingWindow( w3 , now ) );
            verify( Grid::_inBalancingWindow( w4 , now ) );

            // bad input should not stop the balancer

            BSONObj w5; // empty window
            BSONObj w6 = BSON( "activeWindow" << BSON( "start" << 1 ) ); // missing stop
            BSONObj w7 = BSON( "activeWindow" << BSON( "stop" << 1 ) ); // missing start
            BSONObj w8 = BSON( "wrongMarker" << 1 << "start" << 1 << "stop" << 1 ); // active window marker missing
            BSONObj w9 = BSON( "activeWindow" << BSON( "start" << T3 << "stop" << E ) ); // garbage in window

            verify( Grid::_inBalancingWindow( w5 , now ) );
            verify( Grid::_inBalancingWindow( w6 , now ) );
            verify( Grid::_inBalancingWindow( w7 , now ) );
            verify( Grid::_inBalancingWindow( w8 , now ) );
            verify( Grid::_inBalancingWindow( w9 , now ) );

            LOG(1) << "BalancingWidowObjTest passed" << endl;
        }
    } BalancingWindowObjTest;

}
