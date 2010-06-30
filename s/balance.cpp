// balance.cpp

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

#include "../db/jsobj.h"
#include "../db/cmdline.h"

#include "balance.h"
#include "server.h"
#include "shard.h"
#include "config.h"
#include "chunk.h"

namespace mongo {
    
    Balancer balancer;

    Balancer::Balancer() : _balancedLastTime(0), _policy( new BalancerPolicy ){}

    Balancer::~Balancer() {
        delete _policy;
    }

    bool Balancer::_shouldIBalance( DBClientBase& conn ){
        BSONObj x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(2) << "balancer: " << x << endl;
        
        if ( ! x.isEmpty() ){
            if ( x["who"].String() == _myid ){
                log(2) << "balancer: i'm the current balancer" << endl;
                return true;
            }
            
            BSONObj other = conn.findOne( ShardNS::mongos , x["who"].wrap( "_id" ) );
            massert( 13125 , (string)"can't find mongos: " + x["who"].String() , ! other.isEmpty() );

            int secsSincePing = (int)(( jsTime() - other["ping"].Date() ) / 1000 );
            log(2) << "current balancer is: " << other << " ping delay(secs): " << secsSincePing << endl;
            
            if ( secsSincePing < ( 60 * 10 ) ){
                return false;
            }
            
            log() << "balancer: going to take over" << endl;
            // we want to take over, so fall through to below
        }

        // Taking over means replacing 'who' with this balancer's address. Note that
        // to avoid any races, we use a compare-and-set strategy relying on the 
        // incarnation of the previous balancer (the key 'x').

        OID incarnation;
        incarnation.init();
        
        BSONObjBuilder updateQuery;
        updateQuery.append( "_id" , "balancer" );
        if ( x["x"].type() )
            updateQuery.append( x["x"] );
        else
            updateQuery.append( "x" , BSON( "$exists" << false ) );
        
        conn.update( ShardNS::settings , 
                     updateQuery.obj() ,
                     BSON( "$set" << BSON( "who" << _myid << "x" << incarnation ) ) ,
                     true );

        // If another balancer beats this one to the punch, the following query will see 
        // the incarnation for that other guy.
        
        x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log() << "balancer: after update: " << x << endl;
        return _myid == x["who"].String() && incarnation == x["x"].OID();
    }    

    int Balancer::_moveChunks( const vector<CandidateChunkPtr>* candidateChunks ) {
        int movedCount = 0;

        for ( vector<CandidateChunkPtr>::const_iterator it = candidateChunks->begin(); it != candidateChunks->end(); ++it ){
            const CandidateChunk& chunkInfo = *it->get();

            DBConfigPtr cfg = grid.getDBConfig( chunkInfo.ns );
            assert( cfg );
        
            ChunkManagerPtr cm = cfg->getChunkManager( chunkInfo.ns );
            assert( cm );
        
            const BSONObj& chunkToMove = chunkInfo.chunk;
            ChunkPtr c = cm->findChunk( chunkToMove["min"].Obj() );
            if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
                // likely a split happened somewhere
                cm = cfg->getChunkManager( chunkInfo.ns , true );
                assert( cm );

                c = cm->findChunk( chunkToMove["min"].Obj() );
                if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
                    log() << "balancer: chunk mismatch after reload, ignoring will retry issue cm: " 
                          << c->getMin() << " min: " << chunkToMove["min"].Obj() << endl;
                    continue;
                }
            }
        
            string errmsg;
            if ( c->moveAndCommit( Shard::make( chunkInfo.to ) , errmsg ) ){
                movedCount++;
                continue;
            }

            log() << "balancer: MOVE FAILED **** " << errmsg << "\n"
                  << "  from: " << chunkInfo.from << " to: " << " chunk: " << chunkToMove << endl;
        }

        return movedCount;
    }
    
    void Balancer::_ping(){
        assert( _myid.size() && _started );
        try {
            ScopedDbConnection conn( configServer.getPrimary() );
            _ping( conn.conn() );
            conn.done();
        }
        catch ( std::exception& e ){
            log() << "bare ping failed: " << e.what() << endl;
        }
        
    }

    void Balancer::_ping( DBClientBase& conn ){
        WriteConcern w = conn.getWriteConcern();
        conn.setWriteConcern( W_NONE );

        conn.update( ShardNS::mongos , 
                      BSON( "_id" << _myid ) , 
                      BSON( "$set" << BSON( "ping" << DATENOW << "up" << (int)(time(0)-_started) ) ) , 
                      true );

        conn.setWriteConcern( w);
    }
    
    bool Balancer::_checkOIDs(){
        vector<Shard> all;
        Shard::getAllShards( all );
        
        map<int,Shard> oids;
        
        for ( vector<Shard>::iterator i=all.begin(); i!=all.end(); ++i ){
            Shard s = *i;
            BSONObj f = s.runCommand( "admin" , "features" );
            if ( f["oidMachine"].isNumber() ){
                int x = f["oidMachine"].numberInt();
                if ( oids.count(x) == 0 ){
                    oids[x] = s;
                }
                else {
                    log() << "error: 2 machines have " << x << " as oid machine piece " << s.toString() << " and " << oids[x].toString() << endl;
                    s.runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    oids[x].runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    return false;
                }
            }
            else {
                log() << "warning: oidMachine not set on: " << s.toString() << endl;
            }
        }
        return true;
    }

    void Balancer::_doBalanceRound( DBClientBase& conn, vector<CandidateChunkPtr>* candidateChunks ){
        assert( candidateChunks );

        //
        // 1. Check whether there is any sharded collection to be balanced by querying
        // the ShardsNS::database collection
        //
        // { "_id" : "test", "partitioned" : true, "primary" : "shard0",
        //   "sharded" : {
        //       "test.images" : { "key" : { "_id" : 1 }, "unique" : false },
        //       ...  
        //   }
        // }
        //

        auto_ptr<DBClientCursor> cursor = conn.query( ShardNS::database , BSON( "partitioned" << true ) );
        vector< string > collections;
        while ( cursor->more() ){
            BSONObj db = cursor->next();

            // A database may be partitioned but not yet have a sharded collection. 
            // 'cursor' will point to docs that do not contain the "sharded" key. Since 
            // there'd be nothing to balance, we want to skip those here.

            BSONElement shardedColls = db["sharded"];
            if ( shardedColls.eoo() ){
                log(2) << "balancer: skipping database with no sharded collection (" 
                      << db["_id"].str() << ")" << endl;
                continue;
            }
            
            BSONObjIterator i( shardedColls.Obj() );
            while ( i.more() ){
                BSONElement e = i.next();
                collections.push_back( e.fieldName() );
            }
        }
        cursor.reset();

        if ( collections.empty() ) {
            log(1) << "balancer: no collections to balance" << endl;
            return;
        }

        //
        // 2. Get a list of all the shards that are participating in this balance round
        // along with any maximum allowed quotas and current utilization. We get the
        // latter by issuing db.serverStatus() (mem.mapped) to all shards.
        //
        // TODO: skip unresponsive shards and mark information as stale.
        //
 
        vector<Shard> allShards;
        Shard::getAllShards( allShards );
        if ( allShards.size() < 2) {
            log(1) << "balancer: can't balance without more active shards" << endl;
            return;
        }

        map< string, BSONObj > shardLimitsMap; 
        for ( vector<Shard>::const_iterator it = allShards.begin(); it != allShards.end(); ++it ){
            const Shard& s = *it;
            ShardStatus status = s.getStatus();

            BSONObj limitsObj = BSON( ShardFields::maxSize( s.getMaxSize() ) << 
                                      ShardFields::currSize( status.mapped() ) <<
                                      ShardFields::draining( s.isDraining()) );

            shardLimitsMap[ s.getName() ] = limitsObj;
        }

        //
        // 3. For each collection, check if the balancing policy recommends moving anything around.
        //

        for (vector<string>::const_iterator it = collections.begin(); it != collections.end(); ++it ) {
            const string& ns = *it;

            map< string,vector<BSONObj> > shardToChunksMap;
            cursor = conn.query( ShardNS::chunk , QUERY( "ns" << ns ).sort( "min" ) );
            while ( cursor->more() ){
                BSONObj chunk = cursor->next();
                vector<BSONObj>& chunks = shardToChunksMap[chunk["shard"].String()];
                chunks.push_back( chunk.getOwned() );
            }
            cursor.reset();

            if (shardToChunksMap.empty()) {
                log(1) << "balancer: skipping empty collection (" << ns << ")";
                continue;
            }
                
            for ( vector<Shard>::iterator i=allShards.begin(); i!=allShards.end(); ++i ){
                // this just makes sure there is an entry in shardToChunksMap for every shard
                Shard s = *i;
                shardToChunksMap[s.getName()].size();
            }

            CandidateChunk* p = _policy->balance( ns , shardLimitsMap , shardToChunksMap , _balancedLastTime );
            if ( p ) candidateChunks->push_back( CandidateChunkPtr( p ) );
        }
    }

    void Balancer::run(){

        { // init stuff, don't want to do at static init
            StringBuilder buf;
            buf << ourHostname << ":" << cmdLine.port;
            _myid = buf.str();
            log(1) << "balancer myid: " << _myid << endl;
            
            _started = time(0);

            Shard::reloadShardInfo();
        }
        
        _ping();
        _checkOIDs();

        while ( ! inShutdown() ){
            sleepsecs( 10 );
            
            try {
                ScopedDbConnection conn( configServer.getPrimary() );
                _ping( conn.conn() );
                
                if ( ! _checkOIDs() ){
                    uassert( 13258 , "oids broken after resetting!" , _checkOIDs() );
                }
                                    
                vector<CandidateChunkPtr> candidateChunks;
                if ( _shouldIBalance( conn.conn() ) ){
                    log(1) << "balancer: start balancing round" << endl;        
                    candidateChunks.clear();
                    _doBalanceRound( conn.conn() , &candidateChunks );

                    if ( candidateChunks.size() == 0 ) {
                        log(1) << "balancer: no need to move any chunk" << endl;

                    } else {
                        _balancedLastTime = _moveChunks( &candidateChunks );
                        log(1) << "balancer: end balancing round" << endl;        
                    }
                }
                conn.done();
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing balance: " << e.what() << endl;

                // It's possible this shard was removed
                Shard::reloadShardInfo(); 
          
                continue;
            }
        }
    }

}  // namespace mongo
