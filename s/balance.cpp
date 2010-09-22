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

#include "../client/distlock.h"

#include "balance.h"
#include "server.h"
#include "shard.h"
#include "config.h"
#include "chunk.h"
#include "grid.h"

namespace mongo {
    
    Balancer balancer;

    Balancer::Balancer() : _balancedLastTime(0), _policy( new BalancerPolicy ){}

    Balancer::~Balancer() {
        delete _policy;
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
                    log() << "chunk mismatch after reload, ignoring will retry issue cm: " 
                          << c->getMin() << " min: " << chunkToMove["min"].Obj() << endl;
                    continue;
                }
            }
        
            BSONObj res;
            if ( c->moveAndCommit( Shard::make( chunkInfo.to ) , res ) ){
                movedCount++;
                continue;
            }

            log() << "MOVE FAILED **** " << res << "\n"
                  << "           from: " << chunkInfo.from << " to: " << chunkInfo.to << " chunk: " << chunkToMove << endl;

            if ( res["split"].trueValue() ) {
                log() << "move asked for a split of " << c << endl;
                c->split();
            }
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
        // the ShardsNS::collections collection
        //

        auto_ptr<DBClientCursor> cursor = conn.query( ShardNS::collection , BSONObj() );
        vector< string > collections;
        while ( cursor->more() ){
            BSONObj col = cursor->next();

            // sharded collections will have a shard "key".
            if ( ! col["key"].eoo() )
                collections.push_back( col["_id"].String() );
        }
        cursor.reset();

        if ( collections.empty() ) {
            log(1) << "no collections to balance" << endl;
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
            log(1) << "can't balance without more active shards" << endl;
            return;
        }

        map< string, BSONObj > shardLimitsMap; 
        for ( vector<Shard>::const_iterator it = allShards.begin(); it != allShards.end(); ++it ){
            const Shard& s = *it;
            ShardStatus status = s.getStatus();

            BSONObj limitsObj = BSON( ShardFields::maxSize( s.getMaxSize() ) << 
                                      LimitsFields::currSize( status.mapped() ) <<
                                      ShardFields::draining( s.isDraining() )  <<
                                      LimitsFields::hasOpsQueued( status.hasOpsQueued() )
                                    );

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
                log(1) << "skipping empty collection (" << ns << ")";
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
            buf << getHostNameCached() << ":" << cmdLine.port;
            _myid = buf.str();
            log() << "balancer myid: " << _myid << endl;
            
            _started = time(0);

            Shard::reloadShardInfo();
        }
        
        _ping();
        _checkOIDs();

        ConnectionString config = configServer.getConnectionString();
        DistributedLock balanceLock( config , "balancer" );

        while ( ! inShutdown() ){
            
            try {
                ScopedDbConnection conn( config );

                _ping( conn.conn() );                
                if ( ! _checkOIDs() ){
                    uassert( 13258 , "oids broken after resetting!" , _checkOIDs() );
                }
                
                // use fresh shard state
                Shard::reloadShardInfo(); 

                dist_lock_try lk( &balanceLock , "doing balance round" );
                if ( ! lk.got() ){
                    log(1) << "skipping balancing round because another balancer is active" << endl;
                    conn.done();

                    sleepsecs( 30 ); // no need to wake up soon
                    continue;
                }
                        
                if ( ! grid.shouldBalance() ) {
                    log(1) << "skipping balancing round because balancing is disabled" << endl;;
                    conn.done();

                    sleepsecs( 30 );
                    continue;
                }

                log(1) << "*** start balancing round" << endl;        

                vector<CandidateChunkPtr> candidateChunks;
                _doBalanceRound( conn.conn() , &candidateChunks );
                if ( candidateChunks.size() == 0 ) {
                    log(1) << "no need to move any chunk" << endl;
                } else {
                    _balancedLastTime = _moveChunks( &candidateChunks );
                }

                log(1) << "*** end of balancing round" << endl;        
                conn.done();

                sleepsecs( _balancedLastTime ? 5 : 10 );
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing balance: " << e.what() << endl;

                // Just to match the opening statement if in log level 1
                log(1) << "*** End of balancing round" << endl;        

                sleepsecs( 30 ); // sleep a fair amount b/c of error
                continue;
            }
        }
    }

}  // namespace mongo
