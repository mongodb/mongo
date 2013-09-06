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

#include "mongo/pch.h"

#include "mongo/s/balance.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/distlock.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/chunk.h"
#include "mongo/s/config.h"
#include "mongo/s/config_server_checker_service.h"
#include "mongo/s/grid.h"
#include "mongo/s/server.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_tags.h"
#include "mongo/util/version.h"

namespace mongo {

    Balancer balancer;

    Balancer::Balancer() : _balancedLastTime(0), _policy( new BalancerPolicy() ) {}

    Balancer::~Balancer() {
    }

    int Balancer::_moveChunks(const vector<CandidateChunkPtr>* candidateChunks,
                              bool secondaryThrottle,
                              bool waitForDelete)
    {
        int movedCount = 0;

        for ( vector<CandidateChunkPtr>::const_iterator it = candidateChunks->begin(); it != candidateChunks->end(); ++it ) {
            const CandidateChunk& chunkInfo = *it->get();

            // Changes to metadata, borked metadata, and connectivity problems should cause us to
            // abort this chunk move, but shouldn't cause us to abort the entire round of chunks.
            // TODO: Handle all these things more cleanly, since they're expected problems
            try {

                DBConfigPtr cfg = grid.getDBConfig( chunkInfo.ns );
                verify( cfg );

                // NOTE: We purposely do not reload metadata here, since _doBalanceRound already
                // tried to do so once.
                ChunkManagerPtr cm = cfg->getChunkManager( chunkInfo.ns );
                verify( cm );

                ChunkPtr c = cm->findIntersectingChunk( chunkInfo.chunk.min );
                if ( c->getMin().woCompare( chunkInfo.chunk.min ) || c->getMax().woCompare( chunkInfo.chunk.max ) ) {
                    // likely a split happened somewhere
                    cm = cfg->getChunkManager( chunkInfo.ns , true /* reload */);
                    verify( cm );

                    c = cm->findIntersectingChunk( chunkInfo.chunk.min );
                    if ( c->getMin().woCompare( chunkInfo.chunk.min ) || c->getMax().woCompare( chunkInfo.chunk.max ) ) {
                        log() << "chunk mismatch after reload, ignoring will retry issue " << chunkInfo.chunk.toString() << endl;
                        continue;
                    }
                }

                BSONObj res;
                if (c->moveAndCommit(Shard::make(chunkInfo.to),
                                     Chunk::MaxChunkSize,
                                     secondaryThrottle,
                                     waitForDelete,
                                     res)) {
                    movedCount++;
                    continue;
                }

                // the move requires acquiring the collection metadata's lock, which can fail
                log() << "balancer move failed: " << res << " from: " << chunkInfo.from << " to: " << chunkInfo.to
                      << " chunk: " << chunkInfo.chunk << endl;

                if ( res["chunkTooBig"].trueValue() ) {
                    // reload just to be safe
                    cm = cfg->getChunkManager( chunkInfo.ns );
                    verify( cm );
                    c = cm->findIntersectingChunk( chunkInfo.chunk.min );

                    log() << "forcing a split because migrate failed for size reasons" << endl;

                    res = BSONObj();
                    c->singleSplit( true , res );
                    log() << "forced split results: " << res << endl;

                    if ( ! res["ok"].trueValue() ) {
                        log() << "marking chunk as jumbo: " << c->toString() << endl;
                        c->markAsJumbo();
                        // we increment moveCount so we do another round right away
                        movedCount++;
                    }

                }
            }
            catch( const DBException& ex ) {
                warning() << "could not move chunk " << chunkInfo.chunk.toString()
                          << ", continuing balancing round" << causedBy( ex ) << endl;
            }
        }

        return movedCount;
    }

    void Balancer::_ping( DBClientBase& conn, bool waiting ) {
        WriteConcern w = conn.getWriteConcern();
        conn.setWriteConcern( W_NONE );

        conn.update( MongosType::ConfigNS ,
                     BSON( MongosType::name(_myid) ) ,
                     BSON( "$set" << BSON( MongosType::ping(jsTime()) <<
                                           MongosType::up((int)(time(0)-_started)) <<
                                           MongosType::waiting(waiting) <<
                                           MongosType::mongoVersion(versionString) ) ) ,
                     true );

        conn.setWriteConcern( w);
    }

    bool Balancer::_checkOIDs() {
        vector<Shard> all;
        Shard::getAllShards( all );

        map<int,Shard> oids;

        for ( vector<Shard>::iterator i=all.begin(); i!=all.end(); ++i ) {
            Shard s = *i;
            BSONObj f = s.runCommand( "admin" , "features" );
            if ( f["oidMachine"].isNumber() ) {
                int x = f["oidMachine"].numberInt();
                if ( oids.count(x) == 0 ) {
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
    
    /**
     * Occasionally prints a log message with shard versions if the versions are not the same
     * in the cluster.
     */
    void warnOnMultiVersion( const ShardInfoMap& shardInfo ) {

        bool isMultiVersion = false;
        for ( ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i ) {
            if ( !isSameMajorVersion( i->second.getMongoVersion().c_str() ) ) {
                isMultiVersion = true;
                break;
            }
        }

        // If we're all the same version, don't message
        if ( !isMultiVersion ) return;

        warning() << "multiVersion cluster detected, my version is " << versionString << endl;
        for ( ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i ) {
            log() << i->first << " is at version " << i->second.getMongoVersion() << endl;
        }        
    }

    void Balancer::_doBalanceRound( DBClientBase& conn, vector<CandidateChunkPtr>* candidateChunks ) {
        verify( candidateChunks );

        //
        // 1. Check whether there is any sharded collection to be balanced by querying
        // the ShardsNS::collections collection
        //

        auto_ptr<DBClientCursor> cursor = conn.query(CollectionType::ConfigNS, BSONObj());
        vector< string > collections;
        while ( cursor->more() ) {
            BSONObj col = cursor->nextSafe();

            // sharded collections will have a shard "key".
            if ( ! col[CollectionType::keyPattern()].eoo() &&
                 ! col[CollectionType::noBalance()].trueValue() ){
                collections.push_back( col[CollectionType::ns()].String() );
            }
            else if( col[CollectionType::noBalance()].trueValue() ){
                LOG(1) << "not balancing collection " << col[CollectionType::ns()].String()
                       << ", explicitly disabled" << endl;
            }

        }
        cursor.reset();

        if ( collections.empty() ) {
            LOG(1) << "no collections to balance" << endl;
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
            LOG(1) << "can't balance without more active shards" << endl;
            return;
        }
        
        ShardInfoMap shardInfo;
        for ( vector<Shard>::const_iterator it = allShards.begin(); it != allShards.end(); ++it ) {
            const Shard& s = *it;
            ShardStatus status = s.getStatus();
            shardInfo[ s.getName() ] = ShardInfo( s.getMaxSize(),
                                                  status.mapped(),
                                                  s.isDraining(),
                                                  status.hasOpsQueued(),
                                                  s.tags(),
                                                  status.mongoVersion()
                                                  );
        }

        OCCASIONALLY warnOnMultiVersion( shardInfo );

        //
        // 3. For each collection, check if the balancing policy recommends moving anything around.
        //

        for (vector<string>::const_iterator it = collections.begin(); it != collections.end(); ++it ) {
            const string& ns = *it;

            map< string,vector<BSONObj> > shardToChunksMap;
            cursor = conn.query(ChunkType::ConfigNS,
                                QUERY(ChunkType::ns(ns)).sort(ChunkType::min()));

            set<BSONObj> allChunkMinimums;

            while ( cursor->more() ) {
                BSONObj chunk = cursor->nextSafe().getOwned();
                vector<BSONObj>& chunks = shardToChunksMap[chunk[ChunkType::shard()].String()];
                allChunkMinimums.insert( chunk[ChunkType::min()].Obj() );
                chunks.push_back( chunk );
            }
            cursor.reset();

            if (shardToChunksMap.empty()) {
                LOG(1) << "skipping empty collection (" << ns << ")";
                continue;
            }

            for ( vector<Shard>::iterator i=allShards.begin(); i!=allShards.end(); ++i ) {
                // this just makes sure there is an entry in shardToChunksMap for every shard
                Shard s = *i;
                shardToChunksMap[s.getName()].size();
            }

            DistributionStatus status( shardInfo, shardToChunksMap );

            // load tags
            conn.ensureIndex(TagsType::ConfigNS,
                             BSON(TagsType::ns() << 1 << TagsType::min() << 1),
                             true);

            cursor = conn.query(TagsType::ConfigNS,
                                QUERY(TagsType::ns(ns)).sort(TagsType::min()));

            vector<TagRange> ranges;

            while ( cursor->more() ) {
                BSONObj tag = cursor->nextSafe();
                TagRange tr(tag[TagsType::min()].Obj().getOwned(),
                            tag[TagsType::max()].Obj().getOwned(),
                            tag[TagsType::tag()].String());
                ranges.push_back(tr);
                uassert(16356,
                        str::stream() << "tag ranges not valid for: " << ns,
                        status.addTagRange(tr) );

            }
            cursor.reset();

            DBConfigPtr cfg = grid.getDBConfig( ns );
            if ( !cfg ) {
                warning() << "could not load db config to balance " << ns << " collection" << endl;
                continue;
            }

            // This line reloads the chunk manager once if this process doesn't know the collection
            // is sharded yet.
            ChunkManagerPtr cm = cfg->getChunkManagerIfExists( ns, true );
            if ( !cm ) {
                warning() << "could not load chunks to balance " << ns << " collection" << endl;
                continue;
            }

            // loop through tags to make sure no chunk spans tags; splits on tag min. for all chunks
            bool didAnySplits = false;
            for ( unsigned i = 0; i < ranges.size(); i++ ) {
                BSONObj min = ranges[i].min;

                min = cm->getShardKey().extendRangeBound( min, false );

                if ( allChunkMinimums.count( min ) > 0 )
                    continue;

                didAnySplits = true;

                log() << "ns: " << ns << " need to split on "
                      << min << " because there is a range there" << endl;

                ChunkPtr c = cm->findIntersectingChunk( min );

                vector<BSONObj> splitPoints;
                splitPoints.push_back( min );

                BSONObj res;
                if ( !c->multiSplit( splitPoints, res ) ) {
                    error() << "split failed: " << res << endl;
                }
                else {
                    LOG(1) << "split worked: " << res << endl;
                }
                break;
            }

            if ( didAnySplits ) {
                // state change, just wait till next round
                continue;
            }

            CandidateChunk* p = _policy->balance( ns, status, _balancedLastTime );
            if ( p ) candidateChunks->push_back( CandidateChunkPtr( p ) );
        }
    }

    bool Balancer::_init() {
        try {

            log() << "about to contact config servers and shards" << endl;

            // contact the config server and refresh shard information
            // checks that each shard is indeed a different process (no hostname mixup)
            // these checks are redundant in that they're redone at every new round but we want to do them initially here
            // so to catch any problem soon
            Shard::reloadShardInfo();
            _checkOIDs();

            log() << "config servers and shards contacted successfully" << endl;

            StringBuilder buf;
            buf << getHostNameCached() << ":" << cmdLine.port;
            _myid = buf.str();
            _started = time(0);

            log() << "balancer id: " << _myid << " started at " << time_t_to_String_short(_started) << endl;

            return true;

        }
        catch ( std::exception& e ) {
            warning() << "could not initialize balancer, please check that all shards and config servers are up: " << e.what() << endl;
            return false;

        }
    }

    void Balancer::run() {

        // this is the body of a BackgroundJob so if we throw here we're basically ending the balancer thread prematurely
        while ( ! inShutdown() ) {

            if ( ! _init() ) {
                log() << "will retry to initialize balancer in one minute" << endl;
                sleepsecs( 60 );
                continue;
            }

            break;
        }

        int sleepTime = 30;

        // getConnectioString and dist lock constructor does not throw, which is what we expect on while
        // on the balancer thread
        ConnectionString config = configServer.getConnectionString();
        DistributedLock balanceLock( config , "balancer" );

        while ( ! inShutdown() ) {

            try {

                ScopedDbConnection conn(config.toString(), 30);

                // ping has to be first so we keep things in the config server in sync
                _ping( conn.conn() );

                // use fresh shard state
                Shard::reloadShardInfo();

                // refresh chunk size (even though another balancer might be active)
                Chunk::refreshChunkSize();

                BSONObj balancerConfig;
                // now make sure we should even be running
                if ( ! grid.shouldBalance( "", &balancerConfig ) ) {
                    LOG(1) << "skipping balancing round because balancing is disabled" << endl;

                    // Ping again so scripts can determine if we're active without waiting
                    _ping( conn.conn(), true );

                    conn.done();

                    sleepsecs( sleepTime );
                    continue;
                }

                sleepTime = balancerConfig[SettingsType::shortBalancerSleep()].trueValue() ? 30 :
                                                                                             6;
                
                uassert( 13258 , "oids broken after resetting!" , _checkOIDs() );

                {
                    dist_lock_try lk( &balanceLock , "doing balance round" );
                    if ( ! lk.got() ) {
                        LOG(1) << "skipping balancing round because another balancer is active" << endl;

                        // Ping again so scripts can determine if we're active without waiting
                        _ping( conn.conn(), true );

                        conn.done();
                        
                        sleepsecs( sleepTime ); // no need to wake up soon
                        continue;
                    }

                    if ( !isConfigServerConsistent() ) {
                        conn.done();
                        warning() << "Skipping balancing round because data inconsistency"
                                  << " was detected amongst the config servers." << endl;
                        continue;
                    }

                    LOG(1) << "*** start balancing round" << endl;

                    bool waitForDelete = false;
                    if (balancerConfig["_waitForDelete"].trueValue()) {
                        waitForDelete = balancerConfig["_waitForDelete"].trueValue();
                    }

                    bool secondaryThrottle = true; // default to on
                    if ( balancerConfig[SettingsType::secondaryThrottle()].type() ) {
                        secondaryThrottle = balancerConfig[SettingsType::secondaryThrottle()].trueValue();
                    }

                    LOG(1) << "waitForDelete: " << waitForDelete << endl;
                    LOG(1) << "secondaryThrottle: " << secondaryThrottle << endl;

                    vector<CandidateChunkPtr> candidateChunks;
                    _doBalanceRound( conn.conn() , &candidateChunks );
                    if ( candidateChunks.size() == 0 ) {
                        LOG(1) << "no need to move any chunk" << endl;
                        _balancedLastTime = 0;
                    }
                    else {
                        _balancedLastTime = _moveChunks(&candidateChunks,
                                                        secondaryThrottle,
                                                        waitForDelete );
                    }

                    LOG(1) << "*** end of balancing round" << endl;
                }

                // Ping again so scripts can determine if we're active without waiting
                _ping( conn.conn(), true );
                
                conn.done();

                sleepsecs( _balancedLastTime ? sleepTime / 6 : sleepTime );
            }
            catch ( std::exception& e ) {
                log() << "caught exception while doing balance: " << e.what() << endl;

                // Just to match the opening statement if in log level 1
                LOG(1) << "*** End of balancing round" << endl;

                sleepsecs( sleepTime ); // sleep a fair amount b/c of error
                continue;
            }
        }

    }

}  // namespace mongo
