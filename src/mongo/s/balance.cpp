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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balance.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/base/owned_pointer_map.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/config.h"
#include "mongo/s/config_server_checker_service.h"
#include "mongo/s/distlock.h"
#include "mongo/s/grid.h"
#include "mongo/s/server.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_actionlog.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_tags.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::map;
    using std::set;
    using std::string;
    using std::vector;

    MONGO_FP_DECLARE(skipBalanceRound);

    Balancer balancer;

    Balancer::Balancer()
        : _balancedLastTime(0),
          _policy(new BalancerPolicy()) {

    }

    Balancer::~Balancer() {

    }

    int Balancer::_moveChunks(const vector<CandidateChunkPtr>* candidateChunks,
                              const WriteConcernOptions* writeConcern,
                              bool waitForDelete)
    {
        int movedCount = 0;

        for ( vector<CandidateChunkPtr>::const_iterator it = candidateChunks->begin(); it != candidateChunks->end(); ++it ) {

            // If the balancer was disabled since we started this round, don't start new
            // chunks moves.
            SettingsType balancerConfig;
            std::string errMsg;

            if (!grid.getBalancerSettings(&balancerConfig, &errMsg)) {
                warning() << errMsg;
                // No point in continuing the round if the config servers are unreachable.
                return movedCount;
            }

            if ((balancerConfig.isKeySet() && // balancer config doc exists
                    !grid.shouldBalance(balancerConfig)) ||
                    MONGO_FAIL_POINT(skipBalanceRound)) {
                LOG(1) << "Stopping balancing round early as balancing was disabled";
                return movedCount;
            }

            // Changes to metadata, borked metadata, and connectivity problems between shards should
            // cause us to abort this chunk move, but shouldn't cause us to abort the entire round
            // of chunks.
            // TODO(spencer): We probably *should* abort the whole round on issues communicating
            // with the config servers, but its impossible to distinguish those types of failures
            // at the moment.
            // TODO: Handle all these things more cleanly, since they're expected problems
            const CandidateChunk& chunkInfo = *it->get();
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
                                     writeConcern,
                                     waitForDelete,
                                     0, /* maxTimeMS */
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

                    log() << "performing a split because migrate failed for size reasons";

                    Status status = c->split(Chunk::normal, NULL, NULL);
                    log() << "split results: " << status << endl;

                    if ( !status.isOK() ) {
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

    void Balancer::_ping(bool waiting) {
        grid.catalogManager()->update(
                        MongosType::ConfigNS,
                        BSON(MongosType::name(_myid)),
                        BSON("$set" << BSON(MongosType::ping(jsTime()) <<
                                            MongosType::up(static_cast<int>(time(0) - _started)) <<
                                            MongosType::waiting(waiting) <<
                                            MongosType::mongoVersion(versionString))),
                        true,
                        false,
                        NULL);
    }

     /* 
     * Builds the details object for the actionlog.
     * Current formats for detail are:
     * Success: {
     *           "candidateChunks" : ,
     *           "chunksMoved" : ,
     *           "executionTimeMillis" : ,
     *           "errorOccured" : false
     *          }
     * Failure: {
     *           "executionTimeMillis" : ,
     *           "errmsg" : ,
     *           "errorOccured" : true
     *          }
     * @param didError, did this round end in an error?
     * @param executionTime, the time this round took to run
     * @param candidateChunks, the number of chunks identified to be moved
     * @param chunksMoved, the number of chunks moved
     * @param errmsg, the error message for this round
     */

    static BSONObj _buildDetails( bool didError, int executionTime,
            int candidateChunks, int chunksMoved, const std::string& errmsg ) {

        BSONObjBuilder builder;
        builder.append("executionTimeMillis", executionTime);
        builder.append("errorOccured", didError);

        if ( didError ) {
            builder.append("errmsg", errmsg);
        } else {
            builder.append("candidateChunks", candidateChunks);
            builder.append("chunksMoved", chunksMoved);
        }
        return builder.obj();
    }

    /**
     * Reports the result of the balancer round into config.actionlog
     *
     * @param actionLog, which contains the balancer round information to be logged
     *
     */

    static void _reportRound( ActionLogType& actionLog) {
        try {
            ScopedDbConnection conn( configServer.getConnectionString(), 30 );

            // send a copy of the message to the log in case it doesn't reach config.actionlog
            actionLog.setTime(jsTime());

            LOG(1) << "about to log balancer result: " << actionLog;

            // The following method is not thread safe. However, there is only one balancer
            // thread per mongos process. The create collection is a a no-op when the collection
            // already exists
            static bool createActionlog = false;
            if ( ! createActionlog ) {
                try {
                    static const int actionLogSizeBytes = 1024 * 1024 * 2;
                    conn->createCollection( ActionLogType::ConfigNS , actionLogSizeBytes , true );
                }
                catch ( const DBException& ex ) {
                    LOG(1) << "config.actionlog could not be created, another mongos process "
                           << "may have done so" << causedBy(ex);

                }
                createActionlog = true;
            }

            Status result = grid.catalogManager()->insert(ActionLogType::ConfigNS,
                                                          actionLog.toBSON(),
                                                          NULL);
            if ( !result.isOK() ) {
                log() << "Error encountered while logging action from balancer "
                      << result.reason();
            }

            conn.done();
        }
        catch ( const DBException& ex ) {
            // if we got here, it means the config change is only in the log;
            // the change didn't make it to config.actionlog
            warning() << "could not log balancer result" << causedBy(ex);
        }
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

        if ( NULL == cursor.get() ) {
            warning() << "could not query " << CollectionType::ConfigNS
                      << " while trying to balance" << endl;
            return;
        }

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

        ShardInfoMap shardInfo;
        Status loadStatus = DistributionStatus::populateShardInfoMap(&shardInfo);

        if (!loadStatus.isOK()) {
            warning() << "failed to load shard metadata" << causedBy(loadStatus);
            return;
        }

        if (shardInfo.size() < 2) {
            LOG(1) << "can't balance without more active shards";
            return;
        }
        
        OCCASIONALLY warnOnMultiVersion( shardInfo );

        //
        // 3. For each collection, check if the balancing policy recommends moving anything around.
        //

        for (vector<string>::const_iterator it = collections.begin(); it != collections.end(); ++it ) {
            const string& ns = *it;

            OwnedPointerMap<string, OwnedPointerVector<ChunkType> > shardToChunksMap;
            cursor = conn.query(ChunkType::ConfigNS,
                                QUERY(ChunkType::ns(ns)).sort(ChunkType::min()));

            set<BSONObj> allChunkMinimums;

            while ( cursor->more() ) {
                BSONObj chunkDoc = cursor->nextSafe().getOwned();

                StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(chunkDoc);
                if (!chunkRes.isOK()) {
                    error() << "bad chunk format for " << chunkDoc
                            << ": " << chunkRes.getStatus().reason();
                    return;
                }
                auto_ptr<ChunkType> chunk(new ChunkType());
                chunkRes.getValue().cloneTo(chunk.get());

                allChunkMinimums.insert(chunk->getMin().getOwned());
                OwnedPointerVector<ChunkType>*& chunkList =
                        shardToChunksMap.mutableMap()[chunk->getShard()];

                if (chunkList == NULL) {
                    chunkList = new OwnedPointerVector<ChunkType>();
                }

                chunkList->mutableVector().push_back(chunk.release());
            }
            cursor.reset();

            if (shardToChunksMap.map().empty()) {
                LOG(1) << "skipping empty collection (" << ns << ")";
                continue;
            }

            for (ShardInfoMap::const_iterator i = shardInfo.begin(); i != shardInfo.end(); ++i) {
                // this just makes sure there is an entry in shardToChunksMap for every shard
                OwnedPointerVector<ChunkType>*& chunkList =
                        shardToChunksMap.mutableMap()[i->first];

                if (chunkList == NULL) {
                    chunkList = new OwnedPointerVector<ChunkType>();
                }
            }

            DistributionStatus status(shardInfo, shardToChunksMap.map());

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

                min = cm->getShardKeyPattern().getKeyPattern().extendRangeBound( min, false );

                if ( allChunkMinimums.count( min ) > 0 )
                    continue;

                didAnySplits = true;

                log() << "ns: " << ns << " need to split on "
                      << min << " because there is a range there" << endl;

                ChunkPtr c = cm->findIntersectingChunk( min );

                vector<BSONObj> splitPoints;
                splitPoints.push_back( min );

                Status status = c->multiSplit(splitPoints, NULL);
                if ( !status.isOK() ) {
                    error() << "split failed: " << status << endl;
                }
                else {
                    LOG(1) << "split worked" << endl;
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
            buf << getHostNameCached() << ":" << serverGlobalParams.port;
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

        int sleepTime = 10;

        // getConnectioString and dist lock constructor does not throw, which is what we expect on while
        // on the balancer thread
        ConnectionString config = configServer.getConnectionString();

        while ( ! inShutdown() ) {

            Timer balanceRoundTimer;
            ActionLogType actionLog;

            actionLog.setServer(getHostNameCached());
            actionLog.setWhat("balancer.round");

            try {

                ScopedDbConnection conn(config.toString(), 30);

                // ping has to be first so we keep things in the config server in sync
                _ping();

                BSONObj balancerResult;

                // use fresh shard state
                Shard::reloadShardInfo();

                // refresh chunk size (even though another balancer might be active)
                Chunk::refreshChunkSize();

                SettingsType balancerConfig;
                string errMsg;

                if (!grid.getBalancerSettings(&balancerConfig, &errMsg)) {
                    warning() << errMsg;
                    return ;
                }

                // now make sure we should even be running
                if ((balancerConfig.isKeySet() && // balancer config doc exists
                        !grid.shouldBalance(balancerConfig)) ||
                        MONGO_FAIL_POINT(skipBalanceRound)) {

                    LOG(1) << "skipping balancing round because balancing is disabled" << endl;

                    // Ping again so scripts can determine if we're active without waiting
                    _ping( true );

                    conn.done();

                    sleepsecs( sleepTime );
                    continue;
                }

                uassert( 13258 , "oids broken after resetting!" , _checkOIDs() );

                {
                    ScopedDistributedLock balancerLock(config, "balancer");
                    balancerLock.setLockMessage("doing balance round");

                    Status lockStatus = balancerLock.tryAcquire();
                    if (!lockStatus.isOK()) {
                        LOG(1) << "skipping balancing round" << causedBy(lockStatus);

                        // Ping again so scripts can determine if we're active without waiting
                        _ping( true );

                        conn.done();
                        
                        sleepsecs( sleepTime ); // no need to wake up soon
                        continue;
                    }

                    if ( !isConfigServerConsistent() ) {
                        conn.done();
                        warning() << "Skipping balancing round because data inconsistency"
                                  << " was detected amongst the config servers." << endl;
                        sleepsecs( sleepTime );
                        continue;
                    }

                    const bool waitForDelete = (balancerConfig.isWaitForDeleteSet() ?
                            balancerConfig.getWaitForDelete() : false);

                    scoped_ptr<WriteConcernOptions> writeConcern;
                    if (balancerConfig.isKeySet()) { // if balancer doc exists.
                        StatusWith<WriteConcernOptions*> extractStatus =
                                balancerConfig.extractWriteConcern();
                        if (extractStatus.isOK()) {
                            writeConcern.reset(extractStatus.getValue());
                        }
                        else {
                            warning() << extractStatus.getStatus().toString();
                        }
                    }

                    LOG(1) << "*** start balancing round. "
                           << "waitForDelete: " << waitForDelete
                           << ", secondaryThrottle: "
                           << (writeConcern.get() ? writeConcern->toBSON().toString() : "default")
                           << endl;

                    vector<CandidateChunkPtr> candidateChunks;
                    _doBalanceRound( conn.conn() , &candidateChunks );
                    if ( candidateChunks.size() == 0 ) {
                        LOG(1) << "no need to move any chunk" << endl;
                        _balancedLastTime = 0;
                    }
                    else {
                        _balancedLastTime = _moveChunks(&candidateChunks,
                                                        writeConcern.get(),
                                                        waitForDelete );
                    }

                    actionLog.setDetails( _buildDetails( false, balanceRoundTimer.millis(),
                        static_cast<int>(candidateChunks.size()), _balancedLastTime, "") );

                    _reportRound( actionLog );

                    LOG(1) << "*** end of balancing round" << endl;
                }

                // Ping again so scripts can determine if we're active without waiting
                _ping( true );
                
                conn.done();

                sleepsecs( _balancedLastTime ? sleepTime / 10 : sleepTime );
            }
            catch ( std::exception& e ) {
                log() << "caught exception while doing balance: " << e.what() << endl;

                // Just to match the opening statement if in log level 1
                LOG(1) << "*** End of balancing round" << endl;

                // This round failed, tell the world!
                actionLog.setDetails( _buildDetails( true, balanceRoundTimer.millis(),
                    0, 0, e.what()) );

                _reportRound( actionLog );

                sleepsecs( sleepTime ); // sleep a fair amount b/c of error
                continue;
            }
        }

    }

}  // namespace mongo
