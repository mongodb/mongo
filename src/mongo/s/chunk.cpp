/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk.h"

#include <iostream>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/config.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/random.h"
#include "mongo/s/balancer_policy.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::shared_ptr;
    using std::unique_ptr;
    using std::map;
    using std::ostringstream;
    using std::set;
    using std::string;
    using std::stringstream;
    using std::vector;

namespace {

    const int kTooManySplitPoints = 4;

    /**
     * Attempts to move the given chunk to another shard.
     *
     * Returns true if the chunk was actually moved.
     */
    bool tryMoveToOtherShard(const ChunkManager& manager, const ChunkType& chunk) {
        // reload sharding metadata before starting migration
        ChunkManagerPtr chunkMgr = manager.reload(false /* just reloaded in mulitsplit */);

        ShardInfoMap shardInfo;
        Status loadStatus = DistributionStatus::populateShardInfoMap(&shardInfo);

        if (!loadStatus.isOK()) {
            warning() << "failed to load shard metadata while trying to moveChunk after "
                      << "auto-splitting" << causedBy(loadStatus);
            return false;
        }

        if (shardInfo.size() < 2) {
            LOG(0) << "no need to move top chunk since there's only 1 shard";
            return false;
        }

        map<string, vector<ChunkType>> shardToChunkMap;
        DistributionStatus::populateShardToChunksMap(shardInfo, *chunkMgr, &shardToChunkMap);

        StatusWith<string> tagStatus = grid.catalogManager()->getTagForChunk(manager.getns(),
                                                                             chunk);
        if (!tagStatus.isOK()) {
            warning() << "Not auto-moving chunk because of an error encountered while "
                      << "checking tag for chunk: " << tagStatus.getStatus();
            return false;
        }

        DistributionStatus chunkDistribution(shardInfo, shardToChunkMap);
        const string newLocation(
                chunkDistribution.getBestReceieverShard(tagStatus.getValue()));

        if (newLocation.empty()) {
            LOG(1) << "recently split chunk: " << chunk
                   << " but no suitable shard to move to";
            return false;
        }

        if (chunk.getShard() == newLocation) {
            // if this is the best shard, then we shouldn't do anything.
            LOG(1) << "recently split chunk: " << chunk
                   << " already in the best shard";
            return false;
        }

        ChunkPtr toMove = chunkMgr->findIntersectingChunk(chunk.getMin());

        if (!(toMove->getMin() == chunk.getMin() && toMove->getMax() == chunk.getMax())) {
            LOG(1) << "recently split chunk: " << chunk
                   << " modified before we could migrate " << toMove->toString();
            return false;
        }

        log() << "moving chunk (auto): " << toMove->toString() << " to: " << newLocation;

        shared_ptr<Shard> newShard = grid.shardRegistry()->findIfExists(newLocation);
        if (!newShard) {
            warning() << "Newly selected shard " << newLocation << " could not be found.";
            return false;
        }

        BSONObj res;
        WriteConcernOptions noThrottle;
        if (!toMove->moveAndCommit(newShard->getId(),
                                   Chunk::MaxChunkSize,
                                   &noThrottle, /* secondaryThrottle */
                                   false, /* waitForDelete - small chunk, no need */
                                   0, /* maxTimeMS - don't time out */
                                   res)) {
            msgassertedNoTrace(10412, str::stream() << "moveAndCommit failed: " << res);
        }

        // update our config
        manager.reload();

        return true;
    }

} // namespace

    long long Chunk::MaxChunkSize = 1024 * 1024 * 64;
    int Chunk::MaxObjectPerChunk = 250000;

    // Can be overridden from command line
    bool Chunk::ShouldAutoSplit = true;

    Chunk::Chunk(const ChunkManager * manager, BSONObj from)
        : _manager(manager), _lastmod(0, 0, OID()), _dataWritten(mkDataWritten())
    {
        string ns = from.getStringField(ChunkType::ns().c_str());
        _shardId = from.getStringField(ChunkType::shard().c_str());

        _lastmod = ChunkVersion::fromBSON(from[ChunkType::DEPRECATED_lastmod()]);
        verify( _lastmod.isSet() );

        _min = from.getObjectField(ChunkType::min().c_str()).getOwned();
        _max = from.getObjectField(ChunkType::max().c_str()).getOwned();
        
        _jumbo = from[ChunkType::jumbo()].trueValue();

        uassert( 10170 ,  "Chunk needs a ns" , ! ns.empty() );
        uassert( 13327 ,  "Chunk ns must match server ns" , ns == _manager->getns() );

        {
            const auto& shard = grid.shardRegistry()->findIfExists(_shardId);
            uassert(10171, "Chunk needs a server", shard);
        }

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }

    Chunk::Chunk(const ChunkManager * info,
                 const BSONObj& min,
                 const BSONObj& max,
                 const ShardId& shardId,
                 ChunkVersion lastmod)
        : _manager(info),
          _min(min),
          _max(max),
          _shardId(shardId),
          _lastmod(lastmod),
          _jumbo(false),
          _dataWritten(mkDataWritten())
    {}

    int Chunk::mkDataWritten() {
        PseudoRandom r(static_cast<int64_t>(time(0)));
        return r.nextInt32( MaxChunkSize / ChunkManager::SplitHeuristics::splitTestFactor );
    }

    string Chunk::getns() const {
        verify( _manager );
        return _manager->getns();
    }

    bool Chunk::containsKey( const BSONObj& shardKey ) const {
        return getMin().woCompare( shardKey ) <= 0 && shardKey.woCompare( getMax() ) < 0;
    }

    bool ChunkRange::containsKey( const BSONObj& shardKey ) const {
        // same as Chunk method
        return getMin().woCompare( shardKey ) <= 0 && shardKey.woCompare( getMax() ) < 0;
    }

    bool Chunk::_minIsInf() const {
        return 0 ==
            _manager->getShardKeyPattern().getKeyPattern().globalMin().woCompare(getMin());
    }

    bool Chunk::_maxIsInf() const {
        return 0 ==
            _manager->getShardKeyPattern().getKeyPattern().globalMax().woCompare(getMax());
    }

    BSONObj Chunk::_getExtremeKey(bool doSplitAtLower) const {
        Query q;
        if (doSplitAtLower) {
            q.sort( _manager->getShardKeyPattern().toBSON() );
        }
        else {
            // need to invert shard key pattern to sort backwards
            // TODO: make a helper in ShardKeyPattern?

            BSONObj k = _manager->getShardKeyPattern().toBSON();
            BSONObjBuilder r;

            BSONObjIterator i(k);
            while( i.more() ) {
                BSONElement e = i.next();
                uassert( 10163 ,  "can only handle numbers here - which i think is correct" , e.isNumber() );
                r.append( e.fieldName() , -1 * e.number() );
            }

            q.sort( r.obj() );
        }

        // find the extreme key
        ScopedDbConnection conn(_getShardConnectionString());
        BSONObj end;

        if (doSplitAtLower) {
            // Splitting close to the lower bound means that the split point will be the
            // upper bound. Chunk range upper bounds are exclusive so skip a document to
            // make the lower half of the split end up with a single document.
            unique_ptr<DBClientCursor> cursor = conn->query(_manager->getns(),
                                                          q,
                                                          1, /* nToReturn */
                                                          1 /* nToSkip */);

            if (cursor->more()) {
                end = cursor->next().getOwned();
            }
        }
        else {
            end = conn->findOne(_manager->getns(), q);
        }

        conn.done();
        if ( end.isEmpty() )
            return BSONObj();
        return _manager->getShardKeyPattern().extractShardKeyFromDoc(end);
    }

    void Chunk::pickMedianKey( BSONObj& medianKey ) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn(_getShardConnectionString());
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKeyPattern().toBSON() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.appendBool( "force" , true );
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , result )) {
            conn.done();
            ostringstream os;
            os << "splitVector command (median key) failed: " << result;
            uassert( 13503 , os.str() , 0 );
        }

        BSONObjIterator it( result.getObjectField( "splitKeys" ) );
        if ( it.more() ) {
            medianKey = it.next().Obj().getOwned();
        }

        conn.done();
    }

    void Chunk::pickSplitVector(vector<BSONObj>& splitPoints,
                                long long chunkSize /* bytes */,
                                int maxPoints,
                                int maxObjs) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn(_getShardConnectionString());
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKeyPattern().toBSON() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.append( "maxChunkSizeBytes" , chunkSize );
        cmd.append( "maxSplitPoints" , maxPoints );
        cmd.append( "maxChunkObjects" , maxObjs );
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , result )) {
            conn.done();
            ostringstream os;
            os << "splitVector command failed: " << result;
            uassert( 13345 , os.str() , 0 );
        }

        BSONObjIterator it( result.getObjectField( "splitKeys" ) );
        while ( it.more() ) {
            splitPoints.push_back( it.next().Obj().getOwned() );
        }
        conn.done();
    }

    void Chunk::determineSplitPoints(bool atMedian, vector<BSONObj>* splitPoints) const {
        // if splitting is not obligatory we may return early if there are not enough data
        // we cap the number of objects that would fall in the first half (before the split point)
        // the rationale is we'll find a split point without traversing all the data
        if ( atMedian ) {
            BSONObj medianKey;
            pickMedianKey( medianKey );
            if ( ! medianKey.isEmpty() )
                splitPoints->push_back( medianKey );
        }
        else {
            long long chunkSize = _manager->getCurrentDesiredChunkSize();

            // Note: One split point for every 1/2 chunk size.
            const int estNumSplitPoints = _dataWritten / chunkSize * 2;
            if (estNumSplitPoints >= kTooManySplitPoints) {
                // The current desired chunk size will split the chunk into lots of small chunks
                // (At the worst case, this can result into thousands of chunks); so check and
                // see if a bigger value can be used.

                chunkSize = std::min(_dataWritten, Chunk::MaxChunkSize);
            }

            pickSplitVector(*splitPoints, chunkSize, 0, MaxObjectPerChunk);

            if ( splitPoints->size() <= 1 ) {
                // no split points means there isn't enough data to split on
                // 1 split point means we have between half the chunk size to full chunk size
                // so we shouldn't split
                splitPoints->clear();
            }
        }
    }

    Status Chunk::split(SplitPointMode mode, size_t* resultingSplits, BSONObj* res) const {
        size_t dummy;
        if (resultingSplits == NULL) {
            resultingSplits = &dummy;
        }

        bool atMedian = mode == Chunk::atMedian;
        vector<BSONObj> splitPoints;

        determineSplitPoints( atMedian, &splitPoints );
        if (splitPoints.empty()) {
            string msg;
            if (atMedian) {
                msg = "cannot find median in chunk, possibly empty";
            }
            else {
                msg = "chunk not full enough to trigger auto-split";
            }

            LOG(1) << msg;
            return Status(ErrorCodes::CannotSplit, msg);
        }

        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use
        // the very first (or last) key as a split point.
        // This heuristic is skipped for "special" shard key patterns that are not likely to
        // produce monotonically increasing or decreasing values (e.g. hashed shard keys).
        if (mode == Chunk::autoSplitInternal &&
            KeyPattern::isOrderedKeyPattern(_manager->getShardKeyPattern().toBSON())) {

            if (_minIsInf()) {
                BSONObj key = _getExtremeKey(true);
                if (!key.isEmpty()) {
                    splitPoints[0] = key.getOwned();
                }
            }
            else if (_maxIsInf()) {
                BSONObj key = _getExtremeKey(false);
                if (!key.isEmpty()) {
                    splitPoints.pop_back();
                    splitPoints.push_back(key);
                }
            }
        }

        // Normally, we'd have a sound split point here if the chunk is not empty.
        // It's also a good place to sanity check.
        if ( _min == splitPoints.front() ) {
            string msg(str::stream() << "not splitting chunk " << toString()
                                     << ", split point " << splitPoints.front()
                                     << " is exactly on chunk bounds");
            log() << msg;
            return Status(ErrorCodes::CannotSplit, msg);
        }

        if ( _max == splitPoints.back() ) {
            string msg(str::stream() << "not splitting chunk " << toString()
                                     << ", split point " << splitPoints.back()
                                     << " is exactly on chunk bounds");
            log() << msg;
            return Status(ErrorCodes::CannotSplit, msg);
        }

        Status status = multiSplit(splitPoints, res);
        *resultingSplits = splitPoints.size();
        return status;
    }

    Status Chunk::multiSplit(const vector<BSONObj>& m, BSONObj* res) const {
        const size_t maxSplitPoints = 8192;

        uassert( 10165 , "can't split as shard doesn't have a manager" , _manager );
        uassert( 13332 , "need a split key to split chunk" , !m.empty() );
        uassert( 13333 , "can't split a chunk in that many parts", m.size() < maxSplitPoints );
        uassert( 13003 , "can't split a chunk with only one distinct value" , _min.woCompare(_max) );

        ScopedDbConnection conn(_getShardConnectionString());

        BSONObjBuilder cmd;
        cmd.append( "splitChunk" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKeyPattern().toBSON() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.append( "from" , getShardId());
        cmd.append( "splitKeys" , m );
        cmd.append("configdb", grid.catalogManager()->connectionString().toString());
        cmd.append("epoch", _manager->getVersion().epoch());
        BSONObj cmdObj = cmd.obj();

        BSONObj dummy;
        if (res == NULL) {
            res = &dummy;
        }

        if (!conn->runCommand("admin", cmdObj, *res)) {
            string msg(str::stream() << "splitChunk failed - cmd: "
                                     << cmdObj << " result: " << *res);
            warning() << msg;
            conn.done();

            return Status(ErrorCodes::SplitFailed, msg);
        }

        conn.done();
        
        // force reload of config
        _manager->reload();

        return Status::OK();
    }

    bool Chunk::moveAndCommit(const ShardId& toShardId,
                              long long chunkSize /* bytes */,
                              const WriteConcernOptions* writeConcern,
                              bool waitForDelete,
                              int maxTimeMS,
                              BSONObj& res) const {
        uassert(10167,
                "can't move shard to its current location!",
                getShardId() != toShardId);

        log() << "moving chunk ns: " << _manager->getns() << " moving ( " << toString() << ") "
              << getShardId() << " -> " << toShardId;

        const auto& from = grid.shardRegistry()->findIfExists(getShardId());

        BSONObjBuilder builder;
        builder.append("moveChunk", _manager->getns());
        builder.append("from", from->getConnString().toString());
        {
            const auto& toShard = grid.shardRegistry()->findIfExists(toShardId);
            builder.append("to", toShard->getConnString().toString());
        }
        // NEEDED FOR 2.0 COMPATIBILITY
        builder.append("fromShard", from->getId());
        builder.append("toShard", toShardId);
        ///////////////////////////////
        builder.append("min", _min);
        builder.append("max", _max);
        builder.append("maxChunkSizeBytes", chunkSize);
        builder.append("configdb", grid.catalogManager()->connectionString().toString());

        // For legacy secondary throttle setting.
        bool secondaryThrottle = true;
        if (writeConcern &&
                writeConcern->wNumNodes <= 1 &&
                writeConcern->wMode.empty()) {
            secondaryThrottle = false;
        }

        builder.append("secondaryThrottle", secondaryThrottle);

        if (secondaryThrottle && writeConcern) {
            builder.append("writeConcern", writeConcern->toBSON());
        }

        builder.append("waitForDelete", waitForDelete);
        builder.append(LiteParsedQuery::cmdOptionMaxTimeMS, maxTimeMS);
        builder.append("epoch", _manager->getVersion().epoch());

        ScopedDbConnection fromconn(from->getConnString());
        bool worked = fromconn->runCommand("admin", builder.done(), res);
        fromconn.done();

        LOG( worked ? 1 : 0 ) << "moveChunk result: " << res;

        // if succeeded, needs to reload to pick up the new location
        // if failed, mongos may be stale
        // reload is excessive here as the failure could be simply because collection metadata is taken
        _manager->reload();

        return worked;
    }

    bool Chunk::splitIfShould( long dataWritten ) const {
        dassert( ShouldAutoSplit );
        LastError::Disabled d(&LastError::get(cc()));

        try {
            _dataWritten += dataWritten;
            int splitThreshold = getManager()->getCurrentDesiredChunkSize();
            if (_minIsInf() || _maxIsInf()) {
                splitThreshold = (int)((double)splitThreshold * .9);
            }

            if ( _dataWritten < splitThreshold / ChunkManager::SplitHeuristics::splitTestFactor )
                return false;
            
            if ( ! getManager()->_splitHeuristics._splitTickets.tryAcquire() ) {
                LOG(1) << "won't auto split because not enough tickets: " << getManager()->getns();
                return false;
            }
            TicketHolderReleaser releaser( &(getManager()->_splitHeuristics._splitTickets) );

            // this is a bit ugly
            // we need it so that mongos blocks for the writes to actually be committed
            // this does mean mongos has more back pressure than mongod alone
            // since it nots 100% tcp queue bound
            // this was implicit before since we did a splitVector on the same socket
            ShardConnection::sync();

            LOG(1) << "about to initiate autosplit: " << *this << " dataWritten: " << _dataWritten << " splitThreshold: " << splitThreshold;

            BSONObj res;
            size_t splitCount = 0;
            Status status = split(Chunk::autoSplitInternal,
                                  &splitCount,
                                  &res);
            if (!status.isOK()) {
                // Split would have issued a message if we got here. This means there wasn't enough
                // data to split, so don't want to try again until considerable more data
                _dataWritten = 0;
                return false;
            }
            
            if (_maxIsInf() || _minIsInf()) {
                // we don't want to reset _dataWritten since we kind of want to check the other side right away
            }
            else {
                // we're splitting, so should wait a bit
                _dataWritten = 0;
            }

            bool shouldBalance = grid.getConfigShouldBalance();
            if (shouldBalance) {
                auto status = grid.catalogManager()->getCollection(_manager->getns());
                if (!status.isOK()) {
                    log() << "Auto-split for " << _manager->getns()
                          << " failed to load collection metadata due to " << status.getStatus();
                    return false;
                }

                shouldBalance = status.getValue().getAllowBalance();
            }

            log() << "autosplitted " << _manager->getns()
                  << " shard: " << toString()
                  << " into " << (splitCount + 1)
                  << " (splitThreshold " << splitThreshold << ")"
#ifdef MONGO_CONFIG_DEBUG_BUILD
                  << " size: " << getPhysicalSize() // slow - but can be useful when debugging
#endif
                  << ( res["shouldMigrate"].eoo() ? "" : (string)" (migrate suggested" +
                     ( shouldBalance ? ")" : ", but no migrations allowed)" ) );

            // Top chunk optimization - try to move the top chunk out of this shard
            // to prevent the hot spot from staying on a single shard. This is based on
            // the assumption that succeeding inserts will fall on the top chunk.
            BSONElement shouldMigrate = res["shouldMigrate"]; // not in mongod < 1.9.1 but that is ok
            if ( ! shouldMigrate.eoo() && shouldBalance ){
                BSONObj range = shouldMigrate.embeddedObject();

                ChunkType chunkToMove;
                {
                    const auto& shard = grid.shardRegistry()->findIfExists(getShardId());
                    chunkToMove.setShard(shard->toString());
                }
                chunkToMove.setMin(range["min"].embeddedObject());
                chunkToMove.setMax(range["max"].embeddedObject());

                tryMoveToOtherShard(*_manager, chunkToMove);
            }

            return true;

        }
        catch ( DBException& e ) {

            // TODO: Make this better - there are lots of reasons a split could fail
            // Random so that we don't sync up with other failed splits
            _dataWritten = mkDataWritten();

            // if the collection lock is taken (e.g. we're migrating), it is fine for the split to fail.
            warning() << "could not autosplit collection " << _manager->getns() << causedBy( e );
            return false;
        }
    }

    const ConnectionString& Chunk::_getShardConnectionString() const {
        const auto& shard = grid.shardRegistry()->findIfExists(getShardId());
        return shard->getConnString();
    }

    long Chunk::getPhysicalSize() const {
        ScopedDbConnection conn(_getShardConnectionString());

        BSONObj result;
        uassert( 10169 ,  "datasize failed!" , conn->runCommand( "admin" ,
                 BSON( "datasize" << _manager->getns()
                       << "keyPattern" << _manager->getShardKeyPattern().toBSON()
                       << "min" << getMin()
                       << "max" << getMax()
                       << "maxSize" << ( MaxChunkSize + 1 )
                       << "estimate" << true
                     ) , result ) );

        conn.done();
        return (long)result["size"].number();
    }

    void Chunk::appendShortVersion( const char * name , BSONObjBuilder& b ) const {
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.append(ChunkType::min(), _min);
        bb.append(ChunkType::max(), _max);
        bb.done();
    }

    bool Chunk::operator==( const Chunk& s ) const {
        return _min.woCompare( s._min ) == 0 && _max.woCompare( s._max ) == 0;
    }

    void Chunk::serialize(BSONObjBuilder& to,ChunkVersion myLastMod) {

        to.append( "_id" , genID( _manager->getns() , _min ) );

        if ( myLastMod.isSet() ) {
            myLastMod.addToBSON(to, ChunkType::DEPRECATED_lastmod());
        }
        else if ( _lastmod.isSet() ) {
            _lastmod.addToBSON(to, ChunkType::DEPRECATED_lastmod());
        }
        else {
            verify(0);
        }

        to << ChunkType::ns(_manager->getns());
        to << ChunkType::min(_min);
        to << ChunkType::max(_max);
        to << ChunkType::shard(_shardId);
    }

    string Chunk::genID() const {
        return genID(_manager->getns(), _min);
    }

    string Chunk::genID( const string& ns , const BSONObj& o ) {
        StringBuilder buf;
        buf << ns << "-";

        BSONObjIterator i(o);
        while ( i.more() ) {
            BSONElement e = i.next();
            buf << e.fieldName() << "_" << e.toString(false, true);
        }

        return buf.str();
    }

    string Chunk::toString() const {
        stringstream ss;
        ss << ChunkType::ns()                 << ": " << _manager->getns()   << ", "
           << ChunkType::shard()              << ": " << _shardId            << ", "
           << ChunkType::DEPRECATED_lastmod() << ": " << _lastmod.toString() << ", "
           << ChunkType::min()                << ": " << _min                << ", "
           << ChunkType::max()                << ": " << _max;
        return ss.str();
    }

    void Chunk::markAsJumbo() const {
        // set this first
        // even if we can't set it in the db
        // at least this mongos won't try and keep moving
        _jumbo = true;

        Status result = grid.catalogManager()->update(ChunkType::ConfigNS,
                                                      BSON(ChunkType::name(genID())),
                                                      BSON("$set" << BSON(ChunkType::jumbo(true))),
                                                      false,    // upsert
                                                      false,    // multi
                                                      NULL);
        if (!result.isOK()) {
            warning() << "couldn't set jumbo for chunk: " << genID() << result.reason();
        }
    }

    void Chunk::refreshChunkSize() {
        auto chunkSizeSettingsResult =
            grid.catalogManager()->getGlobalSettings(SettingsType::ChunkSizeDocKey);
        if (!chunkSizeSettingsResult.isOK()) {
            log() << chunkSizeSettingsResult.getStatus();
            return;
        }
        SettingsType chunkSizeSettings = chunkSizeSettingsResult.getValue();
        int csize = chunkSizeSettings.getChunkSize();

        LOG(1) << "Refreshing MaxChunkSize: " << csize << "MB";

        if (csize != Chunk::MaxChunkSize/(1024*1024)) {
            log() << "MaxChunkSize changing from " << Chunk::MaxChunkSize/(1024*1024) << "MB"
                                         << " to " << csize << "MB";
        }

        if ( !setMaxChunkSizeSizeMB( csize ) ) {
            warning() << "invalid MaxChunkSize: " << csize;
        }
    }

    bool Chunk::setMaxChunkSizeSizeMB( int newMaxChunkSize ) {
        if ( newMaxChunkSize < 1 )
            return false;
        if ( newMaxChunkSize > 1024 )
            return false;
        MaxChunkSize = newMaxChunkSize * 1024 * 1024;
        return true;
    }

} // namespace mongo
