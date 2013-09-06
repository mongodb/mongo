// @file chunk.cpp

/**
 *    Copyright (C) 2008-2012 10gen Inc.
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

#include "mongo/s/chunk.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/queryutil.h"
#include "mongo/platform/random.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/strategy.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_settings.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/timer.h"

namespace mongo {

    inline bool allOfType(BSONType type, const BSONObj& o) {
        BSONObjIterator it(o);
        while(it.more()) {
            if (it.next().type() != type)
                return false;
        }
        return true;
    }

    // -------  Shard --------

    int Chunk::MaxChunkSize = 1024 * 1024 * 64;
    int Chunk::MaxObjectPerChunk = 250000;

    // Can be overridden from command line
    bool Chunk::ShouldAutoSplit = true;

    Chunk::Chunk(const ChunkManager * manager, BSONObj from)
        : _manager(manager), _lastmod(0, OID()), _dataWritten(mkDataWritten())
    {
        string ns = from.getStringField(ChunkType::ns().c_str());
        _shard.reset(from.getStringField(ChunkType::shard().c_str()));

        _lastmod = ChunkVersion::fromBSON(from[ChunkType::DEPRECATED_lastmod()]);
        verify( _lastmod.isSet() );

        _min = from.getObjectField(ChunkType::min().c_str()).getOwned();
        _max = from.getObjectField(ChunkType::max().c_str()).getOwned();
        
        _jumbo = from[ChunkType::jumbo()].trueValue();

        uassert( 10170 ,  "Chunk needs a ns" , ! ns.empty() );
        uassert( 13327 ,  "Chunk ns must match server ns" , ns == _manager->getns() );

        uassert( 10171 ,  "Chunk needs a server" , _shard.ok() );

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }

    Chunk::Chunk(const ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard, ChunkVersion lastmod)
        : _manager(info), _min(min), _max(max), _shard(shard), _lastmod(lastmod), _jumbo(false), _dataWritten(mkDataWritten())
    {}

    int Chunk::mkDataWritten() {
        PseudoRandom r(static_cast<int64_t>(time(0)));
        return r.nextInt32( MaxChunkSize / ChunkManager::SplitHeuristics::splitTestFactor );
    }

    string Chunk::getns() const {
        verify( _manager );
        return _manager->getns();
    }

    bool Chunk::containsPoint( const BSONObj& point ) const {
        return getMin().woCompare( point ) <= 0 && point.woCompare( getMax() ) < 0;
    }

    bool ChunkRange::containsPoint( const BSONObj& point ) const {
        // same as Chunk method
        return getMin().woCompare( point ) <= 0 && point.woCompare( getMax() ) < 0;
    }

    bool Chunk::minIsInf() const {
        return _manager->getShardKey().globalMin().woCompare( getMin() ) == 0;
    }

    bool Chunk::maxIsInf() const {
        return _manager->getShardKey().globalMax().woCompare( getMax() ) == 0;
    }

    BSONObj Chunk::_getExtremeKey( int sort ) const {
        Query q;
        if ( sort == 1 ) {
            q.sort( _manager->getShardKey().key() );
        }
        else {
            // need to invert shard key pattern to sort backwards
            // TODO: make a helper in ShardKeyPattern?

            BSONObj k = _manager->getShardKey().key();
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
        ScopedDbConnection conn(getShard().getConnString());
        BSONObj end = conn->findOne(_manager->getns(), q);
        conn.done();
        if ( end.isEmpty() )
            return BSONObj();
        return _manager->getShardKey().extractKey( end );
    }

    void Chunk::pickMedianKey( BSONObj& medianKey ) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn(getShard().getConnString());
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
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

    void Chunk::pickSplitVector( vector<BSONObj>& splitPoints , int chunkSize /* bytes */, int maxPoints, int maxObjs ) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn(getShard().getConnString());
        BSONObj result;
        BSONObjBuilder cmd;
        cmd.append( "splitVector" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
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

    BSONObj Chunk::singleSplit( bool force , BSONObj& res ) const {
        vector<BSONObj> splitPoint;

        // if splitting is not obligatory we may return early if there are not enough data
        // we cap the number of objects that would fall in the first half (before the split point)
        // the rationale is we'll find a split point without traversing all the data
        if ( ! force ) {
            vector<BSONObj> candidates;
            const int maxPoints = 2;
            pickSplitVector( candidates , getManager()->getCurrentDesiredChunkSize() , maxPoints , MaxObjectPerChunk );
            if ( candidates.size() <= 1 ) {
                // no split points means there isn't enough data to split on
                // 1 split point means we have between half the chunk size to full chunk size
                // so we shouldn't split
                LOG(1) << "chunk not full enough to trigger auto-split " << ( candidates.size() == 0 ? "no split entry" : candidates[0].toString() ) << endl;
                return BSONObj();
            }

            splitPoint.push_back( candidates.front() );

        }
        else {
            // if forcing a split, use the chunk's median key
            BSONObj medianKey;
            pickMedianKey( medianKey );
            if ( ! medianKey.isEmpty() )
                splitPoint.push_back( medianKey );
        }

        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use
        // the very first (or last) key as a split point.
        // This heuristic is skipped for "special" shard key patterns that are not likely to
        // produce monotonically increasing or decreasing values (e.g. hashed shard keys).
        // TODO: need better way to detect when shard keys vals are increasing/decreasing, and
        // use that better method to determine whether to apply heuristic here.
        if ( ! skey().isSpecial() ){
            if ( minIsInf() ) {
                splitPoint.clear();
                BSONObj key = _getExtremeKey( 1 );
                if ( ! key.isEmpty() ) {
                    splitPoint.push_back( key );
                }
            }
            else if ( maxIsInf() ) {
                splitPoint.clear();
                BSONObj key = _getExtremeKey( -1 );
                if ( ! key.isEmpty() ) {
                    splitPoint.push_back( key );
                }
            }
        }

        // Normally, we'd have a sound split point here if the chunk is not empty. It's also a good place to
        // sanity check.
        if ( splitPoint.empty() || _min == splitPoint.front() || _max == splitPoint.front() ) {
            log() << "want to split chunk, but can't find split point chunk " << toString()
                  << " got: " << ( splitPoint.empty() ? "<empty>" : splitPoint.front().toString() ) << endl;
            return BSONObj();
        }
        
        if (multiSplit( splitPoint , res ))
            return splitPoint.front();
        else
            return BSONObj();
    }

    bool Chunk::multiSplit( const vector<BSONObj>& m , BSONObj& res ) const {
        const size_t maxSplitPoints = 8192;

        uassert( 10165 , "can't split as shard doesn't have a manager" , _manager );
        uassert( 13332 , "need a split key to split chunk" , !m.empty() );
        uassert( 13333 , "can't split a chunk in that many parts", m.size() < maxSplitPoints );
        uassert( 13003 , "can't split a chunk with only one distinct value" , _min.woCompare(_max) );

        ScopedDbConnection conn(getShard().getConnString());

        BSONObjBuilder cmd;
        cmd.append( "splitChunk" , _manager->getns() );
        cmd.append( "keyPattern" , _manager->getShardKey().key() );
        cmd.append( "min" , getMin() );
        cmd.append( "max" , getMax() );
        cmd.append( "from" , getShard().getName() );
        cmd.append( "splitKeys" , m );
        cmd.append( "shardId" , genID() );
        cmd.append( "configdb" , configServer.modelServer() );
        BSONObj cmdObj = cmd.obj();

        if ( ! conn->runCommand( "admin" , cmdObj , res )) {
            warning() << "splitChunk failed - cmd: " << cmdObj << " result: " << res << endl;
            conn.done();

            // Mark the minor version for *eventual* reload
            _manager->markMinorForReload( this->_lastmod );

            return false;
        }

        conn.done();
        
        // force reload of config
        _manager->reload();

        return true;
    }

    bool Chunk::moveAndCommit(const Shard& to,
                              long long chunkSize /* bytes */,
                              bool secondaryThrottle,
                              bool waitForDelete,
                              BSONObj& res) const
    {
        uassert( 10167 ,  "can't move shard to its current location!" , getShard() != to );

        log() << "moving chunk ns: " << _manager->getns() << " moving ( " << toString() << ") " << _shard.toString() << " -> " << to.toString() << endl;

        Shard from = _shard;

        ScopedDbConnection fromconn(from.getConnString());

        bool worked = fromconn->runCommand( "admin" ,
                                            BSON( "moveChunk" << _manager->getns() <<
                                                  "from" << from.getAddress().toString() <<
                                                  "to" << to.getAddress().toString() <<
                                                  // NEEDED FOR 2.0 COMPATIBILITY
                                                  "fromShard" << from.getName() <<
                                                  "toShard" << to.getName() <<
                                                  ///////////////////////////////
                                                  "min" << _min <<
                                                  "max" << _max <<
                                                  "maxChunkSizeBytes" << chunkSize <<
                                                  "shardId" << genID() <<
                                                  "configdb" << configServer.modelServer() <<
                                                  "secondaryThrottle" << secondaryThrottle <<
                                                  "waitForDelete" << waitForDelete
                                                   ) ,
                                            res);
        fromconn.done();

        LOG( worked ? 1 : 0 ) << "moveChunk result: " << res << endl;

        // if succeeded, needs to reload to pick up the new location
        // if failed, mongos may be stale
        // reload is excessive here as the failure could be simply because collection metadata is taken
        _manager->reload();

        return worked;
    }

    bool Chunk::splitIfShould( long dataWritten ) const {
        LastError::Disabled d( lastError.get() );

        try {
            _dataWritten += dataWritten;
            int splitThreshold = getManager()->getCurrentDesiredChunkSize();
            if ( minIsInf() || maxIsInf() ) {
                splitThreshold = (int) ((double)splitThreshold * .9);
            }

            if ( _dataWritten < splitThreshold / ChunkManager::SplitHeuristics::splitTestFactor )
                return false;
            
            if ( ! getManager()->_splitHeuristics._splitTickets.tryAcquire() ) {
                LOG(1) << "won't auto split because not enough tickets: " << getManager()->getns() << endl;
                return false;
            }
            TicketHolderReleaser releaser( &(getManager()->_splitHeuristics._splitTickets) );

            // this is a bit ugly
            // we need it so that mongos blocks for the writes to actually be committed
            // this does mean mongos has more back pressure than mongod alone
            // since it nots 100% tcp queue bound
            // this was implicit before since we did a splitVector on the same socket
            ShardConnection::sync();

            LOG(1) << "about to initiate autosplit: " << *this << " dataWritten: " << _dataWritten << " splitThreshold: " << splitThreshold << endl;

            BSONObj res;
            BSONObj splitPoint = singleSplit( false /* does not force a split if not enough data */ , res );
            if ( splitPoint.isEmpty() ) {
                // singleSplit would have issued a message if we got here
                _dataWritten = 0; // this means there wasn't enough data to split, so don't want to try again until considerable more data
                return false;
            }
            
            if ( maxIsInf() || minIsInf() ) {
                // we don't want to reset _dataWritten since we kind of want to check the other side right away
            }
            else {
                _dataWritten = 0; // we're splitting, so should wait a bit
            }

            bool shouldBalance = grid.shouldBalance( _manager->getns() );

            log() << "autosplitted " << _manager->getns() << " shard: " << toString()
                  << " on: " << splitPoint << " (splitThreshold " << splitThreshold << ")"
#ifdef _DEBUG
                  << " size: " << getPhysicalSize() // slow - but can be useful when debugging
#endif
                  << ( res["shouldMigrate"].eoo() ? "" : (string)" (migrate suggested" +
                     ( shouldBalance ? ")" : ", but no migrations allowed)" ) ) << endl;

            BSONElement shouldMigrate = res["shouldMigrate"]; // not in mongod < 1.9.1 but that is ok
            if ( ! shouldMigrate.eoo() && shouldBalance ){
                BSONObj range = shouldMigrate.embeddedObject();
                BSONObj min = range["min"].embeddedObject();
                BSONObj max = range["max"].embeddedObject();
                
                // reload sharding metadata before starting migration
                Shard::reloadShardInfo();

                Shard newLocation = Shard::pick( getShard() );
                if ( getShard() == newLocation ) {
                    // if this is the best shard, then we shouldn't do anything (Shard::pick already logged our shard).
                    LOG(1) << "recently split chunk: " << range << " already in the best shard: " << getShard() << endl;
                    return true; // we did split even if we didn't migrate
                }

                ChunkManagerPtr cm = _manager->reload(false/*just reloaded in mulitsplit*/);
                ChunkPtr toMove = cm->findIntersectingChunk(min);

                if ( ! (toMove->getMin() == min && toMove->getMax() == max) ){
                    LOG(1).stream() << "recently split chunk: " << range << " modified before we could migrate " << toMove << endl;
                    return true;
                }

                log().stream() << "moving chunk (auto): " << toMove << " to: " << newLocation.toString() << endl;

                BSONObj res;
                massert( 10412 ,
                         str::stream() << "moveAndCommit failed: " << res ,
                         toMove->moveAndCommit( newLocation , 
                                                MaxChunkSize , 
                                                false , /* secondaryThrottle - small chunk, no need */
                                                false, /* waitForDelete - small chunk, no need */
                                                res ) );
                
                // update our config
                _manager->reload();
            }

            return true;

        }
        catch ( DBException& e ) {

            // TODO: Make this better - there are lots of reasons a split could fail
            // Random so that we don't sync up with other failed splits
            _dataWritten = mkDataWritten();

            // if the collection lock is taken (e.g. we're migrating), it is fine for the split to fail.
            warning() << "could not autosplit collection " << _manager->getns() << causedBy( e ) << endl;
            return false;
        }
    }

    long Chunk::getPhysicalSize() const {
        ScopedDbConnection conn(getShard().getConnString());

        BSONObj result;
        uassert( 10169 ,  "datasize failed!" , conn->runCommand( "admin" ,
                 BSON( "datasize" << _manager->getns()
                       << "keyPattern" << _manager->getShardKey().key()
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
        to << ChunkType::shard(_shard.getName());
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
           << ChunkType::shard()              << ": " << _shard.toString()   << ", "
           << ChunkType::DEPRECATED_lastmod() << ": " << _lastmod.toString() << ", "
           << ChunkType::min()                << ": " << _min                << ", "
           << ChunkType::max()                << ": " << _max;
        return ss.str();
    }

    ShardKeyPattern Chunk::skey() const {
        return _manager->getShardKey();
    }

    void Chunk::markAsJumbo() const {
        // set this first
        // even if we can't set it in the db
        // at least this mongos won't try and keep moving
        _jumbo = true;

        try {
            ScopedDbConnection conn(configServer.modelServer(), 30);

            conn->update(ChunkType::ConfigNS,
                         BSON(ChunkType::name(genID())),
                         BSON("$set" << BSON(ChunkType::jumbo(true))));
            conn.done();
        }
        catch ( DBException& e ) {
            warning() << "couldn't set jumbo for chunk: " << genID() << causedBy( e ) << endl;
        }
    }

    void Chunk::refreshChunkSize() {
        BSONObj o = grid.getConfigSetting("chunksize");

        if ( o.isEmpty() ) {
           return;
        }

        int csize = o[SettingsType::chunksize()].numberInt();

        // validate chunksize before proceeding
        if ( csize == 0 ) {
            // setting was not modified; mark as such
            log() << "warning: invalid chunksize (" << csize << ") ignored" << endl;
            return;
        }

        LOG(1) << "Refreshing MaxChunkSize: " << csize << "MB" << endl;

        if (csize != Chunk::MaxChunkSize/(1024*1024)) {
            log() << "MaxChunkSize changing from " << Chunk::MaxChunkSize/(1024*1024) << "MB"
                                         << " to " << csize << "MB" << endl;
        }

        if ( !setMaxChunkSizeSizeMB( csize ) ) {
            warning() << "invalid MaxChunkSize: " << csize << endl;
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

    // -------  ChunkManager --------

    AtomicUInt ChunkManager::NextSequenceNumber = 1;

    ChunkManager::ChunkManager( const string& ns, const ShardKeyPattern& pattern , bool unique ) :
        _ns( ns ),
        _key( pattern ),
        _unique( unique ),
        _chunkRanges(),
        _mutex("ChunkManager"),
        _sequenceNumber(++NextSequenceNumber)
    {
        //
        // Sets up a chunk manager from new data
        //
    }

    ChunkManager::ChunkManager( const BSONObj& collDoc ) :
        // Need the ns early, to construct the lock
        // TODO: Construct lock on demand?  Not sure why we need to keep it around
        _ns(collDoc[CollectionType::ns()].type() == String ?
                                                        collDoc[CollectionType::ns()].String() :
                                                        ""),
        _key(collDoc[CollectionType::keyPattern()].type() == Object ?
                                                        collDoc[CollectionType::keyPattern()].Obj().getOwned() :
                                                        BSONObj()),
        _unique(collDoc[CollectionType::unique()].trueValue()),
        _chunkRanges(),
        _mutex("ChunkManager"),
        // The shard versioning mechanism hinges on keeping track of the number of times we reloaded ChunkManager's.
        // Increasing this number here will prompt checkShardVersion() to refresh the connection-level versions to
        // the most up to date value.
        _sequenceNumber(++NextSequenceNumber)
    {

        //
        // Sets up a chunk manager from an existing sharded collection document
        //

        verify( _ns != ""  );
        verify( ! _key.key().isEmpty() );

        _version = ChunkVersion::fromBSON( collDoc );
    }

    ChunkManager::ChunkManager( ChunkManagerPtr oldManager ) :
        _ns( oldManager->getns() ),
        _key( oldManager->getShardKey() ),
        _unique( oldManager->isUnique() ),
        _chunkRanges(),
        _mutex("ChunkManager"),
        _sequenceNumber(++NextSequenceNumber)
    {
        //
        // Sets up a chunk manager based on an older manager
        //

        _oldManager = oldManager;
    }

    void ChunkManager::loadExistingRanges( const string& config ){

        int tries = 3;
        while (tries--) {
            ChunkMap chunkMap;
            set<Shard> shards;
            ShardVersionMap shardVersions;
            Timer t;

            bool success = _load( config, chunkMap, shards, shardVersions, _oldManager );

            if( success ){
                {
                    int ms = t.millis();
                    log() << "ChunkManager: time to load chunks for " << _ns << ": " << ms << "ms"
                          << " sequenceNumber: " << _sequenceNumber
                          << " version: " << _version.toString()
                          << " based on: " <<
                           ( _oldManager.get() ? _oldManager->getVersion().toString() : "(empty)" )
                          << endl;
                }

                // TODO: Merge into diff code above, so we validate in one place
                if (_isValid(chunkMap)) {
                    // These variables are const for thread-safety. Since the
                    // constructor can only be called from one thread, we don't have
                    // to worry about that here.
                    const_cast<ChunkMap&>(_chunkMap).swap(chunkMap);
                    const_cast<set<Shard>&>(_shards).swap(shards);
                    const_cast<ShardVersionMap&>(_shardVersions).swap(shardVersions);
                    const_cast<ChunkRangeManager&>(_chunkRanges).reloadAll(_chunkMap);

                    // Once we load data, clear reference to old manager
                    _oldManager.reset();

                    return;
                }
            }

            if (_chunkMap.size() < 10) {
                _printChunks();
            }
            
            warning() << "ChunkManager loaded an invalid config for " << _ns
                      << ", trying again" << endl;

            sleepmillis(10 * (3-tries));
        }

        // this will abort construction so we should never have a reference to an invalid config
        msgasserted(13282, "Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.");
    }


    /**
     * This is an adapter so we can use config diffs - mongos and mongod do them slightly
     * differently
     *
     * The mongos adapter here tracks all shards, and stores ranges by (max, Chunk) in the map.
     */
    class CMConfigDiffTracker : public ConfigDiffTracker<ChunkPtr,Shard> {
    public:
        CMConfigDiffTracker( ChunkManager* manager ) : _manager( manager ) {}

        virtual bool isTracked( const BSONObj& chunkDoc ) const {
            // Mongos tracks all shards
            return true;
        }

        virtual BSONObj minFrom( const ChunkPtr& val ) const {
            return val.get()->getMin();
        }

        virtual bool isMinKeyIndexed() const { return false; }

        virtual pair<BSONObj,ChunkPtr> rangeFor( const BSONObj& chunkDoc, const BSONObj& min, const BSONObj& max ) const {
            ChunkPtr c( new Chunk( _manager, chunkDoc ) );
            return make_pair( max, c );
        }

        virtual Shard shardFor( const string& name ) const {
            return Shard::make( name );
        }

        virtual string nameFrom( const Shard& shard ) const {
            return shard.getName();
        }

        ChunkManager* _manager;

    };

    bool ChunkManager::_load( const string& config,
                              ChunkMap& chunkMap,
                              set<Shard>& shards,
                              ShardVersionMap& shardVersions,
                              ChunkManagerPtr oldManager)
    {

        // Reset the max version, but not the epoch, when we aren't loading from the oldManager
        _version = ChunkVersion( 0, _version.epoch() );
        set<ChunkVersion> minorVersions;

        // If we have a previous version of the ChunkManager to work from, use that info to reduce
        // our config query
        if( oldManager && oldManager->getVersion().isSet() ){

            // Get the old max version
            _version = oldManager->getVersion();
            // Load a copy of the old versions
            shardVersions = oldManager->_shardVersions;

            // Load a copy of the chunk map, replacing the chunk manager with our own
            const ChunkMap& oldChunkMap = oldManager->getChunkMap();

            // Could be v.expensive
            // TODO: If chunks were immutable and didn't reference the manager, we could do more
            // interesting things here
            for( ChunkMap::const_iterator it = oldChunkMap.begin(); it != oldChunkMap.end(); it++ ){

                ChunkPtr oldC = it->second;
                ChunkPtr c( new Chunk( this, oldC->getMin(),
                                             oldC->getMax(),
                                             oldC->getShard(),
                                             oldC->getLastmod() ) );

                c->setBytesWritten( oldC->getBytesWritten() );

                chunkMap.insert( make_pair( oldC->getMax(), c ) );
            }

            // Also get any minor versions stored for reload
            oldManager->getMarkedMinorVersions( minorVersions );

            LOG(2) << "loading chunk manager for collection " << _ns
                   << " using old chunk manager w/ version " << _version.toString()
                   << " and " << oldChunkMap.size() << " chunks" << endl;
        }

        // Attach a diff tracker for the versioned chunk data
        CMConfigDiffTracker differ( this );
        differ.attach( _ns, chunkMap, _version, shardVersions );

        // Diff tracker should *always* find at least one chunk if collection exists
        int diffsApplied = differ.calculateConfigDiff( config, minorVersions );
        if( diffsApplied > 0 ){

            LOG(2) << "loaded " << diffsApplied << " chunks into new chunk manager for " << _ns
                   << " with version " << _version << endl;

            // Add all the shards we find to the shards set
            for( ShardVersionMap::iterator it = shardVersions.begin(); it != shardVersions.end(); it++ ){
                shards.insert( it->first );
            }

            return true;
        }
        else if( diffsApplied == 0 ){

            // No chunks were found for the ns
            warning() << "no chunks found when reloading " << _ns
                      << ", previous version was " << _version << endl;

            // Set all our data to empty
            chunkMap.clear();
            shardVersions.clear();
            _version = ChunkVersion( 0, OID() );

            return true;
        }
        else { // diffsApplied < 0

            bool allInconsistent = differ.numValidDiffs() == 0;

            if( allInconsistent ){
                // All versions are different, this can be normal
                warning() << "major change in chunk information found when reloading "
                          << _ns << ", previous version was " << _version << endl;
            }
            else {
                // Inconsistent load halfway through (due to yielding cursor during load)
                // should be rare
                warning() << "inconsistent chunks found when reloading "
                          << _ns << ", previous version was " << _version
                          << ", this should be rare" << endl;
            }

            // Set all our data to empty to be extra safe
            chunkMap.clear();
            shardVersions.clear();
            _version = ChunkVersion( 0, OID() );

            return allInconsistent;
        }

    }

    ChunkManagerPtr ChunkManager::reload(bool force) const {
        return grid.getDBConfig(getns())->getChunkManager(getns(), force);
    }

    void ChunkManager::markMinorForReload( ChunkVersion majorVersion ) const {
        _splitHeuristics.markMinorForReload( getns(), majorVersion );
    }

    void ChunkManager::getMarkedMinorVersions( set<ChunkVersion>& minorVersions ) const {
        _splitHeuristics.getMarkedMinorVersions( minorVersions );
    }

    void ChunkManager::SplitHeuristics::markMinorForReload( const string& ns, ChunkVersion majorVersion ) {

        // When we get a stale minor version, it means that some *other* mongos has just split a
        // chunk into a number of smaller parts, so we shouldn't need reload the data needed to
        // split it ourselves for awhile.  Don't be very aggressive reloading just because of this,
        // since reloads are expensive and disrupt operations.

        // *** Multiple threads could indicate a stale minor version simultaneously ***
        // TODO:  Ideally, this could be a single-threaded background service doing splits
        // TODO:  Ideally, we wouldn't need to care that this is stale at all
        bool forceReload = false;
        {
            scoped_lock lk( _staleMinorSetMutex );

            // The major version of the chunks which need to be reloaded
            _staleMinorSet.insert( majorVersion );

            // Increment the number of requests for minor version data
            _staleMinorCount++;

            if( _staleMinorCount >= staleMinorReloadThreshold ){

                _staleMinorCount = 0;

                // There's maxParallelSplits coming down this codepath at once -
                // block as little as possible.
                forceReload = true;
            }

            // There is no guarantee that a minor version change will be processed here, in the
            // case where the request comes in "late" and the version's already getting reloaded -
            // it's a heuristic anyway though, and we'll see requests multiple times.
        }

        if( forceReload )
            grid.getDBConfig( ns )->getChunkManagerIfExists( ns, true, true );
    }

    void ChunkManager::SplitHeuristics::getMarkedMinorVersions( set<ChunkVersion>& minorVersions ) {
        scoped_lock lk( _staleMinorSetMutex );
        for( set<ChunkVersion>::iterator it = _staleMinorSet.begin(); it != _staleMinorSet.end(); it++ ){
            minorVersions.insert( *it );
        }
    }

    bool ChunkManager::_isValid(const ChunkMap& chunkMap) {
#define ENSURE(x) do { if(!(x)) { log() << "ChunkManager::_isValid failed: " #x << endl; return false; } } while(0)

        if (chunkMap.empty())
            return true;

        // Check endpoints
        ENSURE(allOfType(MinKey, chunkMap.begin()->second->getMin()));
        ENSURE(allOfType(MaxKey, boost::prior(chunkMap.end())->second->getMax()));

        // Make sure there are no gaps or overlaps
        for (ChunkMap::const_iterator it=boost::next(chunkMap.begin()), end=chunkMap.end(); it != end; ++it) {
            ChunkMap::const_iterator last = boost::prior(it);

            if (!(it->second->getMin() == last->second->getMax())) {
                PRINT(it->second->toString());
                PRINT(it->second->getMin());
                PRINT(last->second->getMax());
            }
            ENSURE(it->second->getMin() == last->second->getMax());
        }

        return true;

#undef ENSURE
    }

    void ChunkManager::_printChunks() const {
        for (ChunkMap::const_iterator it=_chunkMap.begin(), end=_chunkMap.end(); it != end; ++it) {
            log() << *it->second << endl;
        }
    }

    bool ChunkManager::hasShardKey( const BSONObj& obj ) const {
        return _key.hasShardKey( obj );
    }

    void ChunkManager::calcInitSplitsAndShards( const Shard& primary,
                                                const vector<BSONObj>* initPoints,
                                                const vector<Shard>* initShards,
                                                vector<BSONObj>* splitPoints,
                                                vector<Shard>* shards ) const
    {
        verify( _chunkMap.size() == 0 );

        unsigned long long numObjects = 0;
        Chunk c(this, _key.globalMin(), _key.globalMax(), primary);

        if ( !initPoints || !initPoints->size() ) {
            // discover split points
            {
                // get stats to see if there is any data
                ScopedDbConnection shardConn(primary.getConnString());

                numObjects = shardConn->count( getns() );
                shardConn.done();
            }

            if ( numObjects > 0 )
                c.pickSplitVector( *splitPoints , Chunk::MaxChunkSize );

            // since docs alread exists, must use primary shard
            shards->push_back( primary );
        } else {
            // make sure points are unique and ordered
            set<BSONObj> orderedPts;
            for ( unsigned i = 0; i < initPoints->size(); ++i ) {
                BSONObj pt = (*initPoints)[i];
                orderedPts.insert( pt );
            }
            for ( set<BSONObj>::iterator it = orderedPts.begin(); it != orderedPts.end(); ++it ) {
                splitPoints->push_back( *it );
            }

            if ( !initShards || !initShards->size() ) {
                // If not specified, only use the primary shard (note that it's not safe for mongos
                // to put initial chunks on other shards without the primary mongod knowing).
                shards->push_back( primary );
            } else {
                std::copy( initShards->begin() , initShards->end() , std::back_inserter(*shards) );
            }
        }
    }

    void ChunkManager::createFirstChunks( const string& config,
                                          const Shard& primary,
                                          const vector<BSONObj>* initPoints,
                                          const vector<Shard>* initShards )
    {
        // TODO distlock?
        // TODO: Race condition if we shard the collection and insert data while we split across
        // the non-primary shard.

        vector<BSONObj> splitPoints;
        vector<Shard> shards;

        calcInitSplitsAndShards( primary, initPoints, initShards,
                                 &splitPoints, &shards );

        // this is the first chunk; start the versioning from scratch
        ChunkVersion version;
        version.incEpoch();
        version.incMajor();
        
        log() << "going to create " << splitPoints.size() + 1 << " chunk(s) for: " << _ns
              << " using new epoch " << version.epoch() << endl;
        
        ScopedDbConnection conn(config, 30);

        // Make sure we don't have any chunks that already exist here
        unsigned long long existingChunks =
            conn->count(ChunkType::ConfigNS, BSON(ChunkType::ns(_ns)));

        uassert( 13449 , str::stream() << "collection " << _ns << " already sharded with "
                                       << existingChunks << " chunks", existingChunks == 0 );

        for ( unsigned i=0; i<=splitPoints.size(); i++ ) {
            BSONObj min = i == 0 ? _key.globalMin() : splitPoints[i-1];
            BSONObj max = i < splitPoints.size() ? splitPoints[i] : _key.globalMax();

            Chunk temp( this , min , max , shards[ i % shards.size() ], version );

            BSONObjBuilder chunkBuilder;
            temp.serialize( chunkBuilder );
            BSONObj chunkObj = chunkBuilder.obj();

            conn->update(ChunkType::ConfigNS,
                         QUERY(ChunkType::name(temp.genID())),
                         chunkObj,
                         true,
                         false );

            version.incMinor();
        }

        string errmsg = conn->getLastError();
        if ( errmsg.size() ) {
            string ss = str::stream() << "creating first chunks failed. result: " << errmsg;
            error() << ss << endl;
            msgasserted( 15903 , ss );
        }
        
        conn.done();

        _version = ChunkVersion( 0, version.epoch() );
    }

    ChunkPtr ChunkManager::findIntersectingChunk( const BSONObj& point ) const {
        {
            BSONObj foo;
            ChunkPtr c;
            {
                ChunkMap::const_iterator it = _chunkMap.upper_bound( point );
                if (it != _chunkMap.end()) {
                    foo = it->first;
                    c = it->second;
                }
            }

            if ( c ) {
                if ( c->containsPoint( point ) ){
                    dassert( c->containsPoint( point ) ); // doesn't use fast-path in extractKey
                    return c;
                }

                PRINT(foo);
                PRINT(*c);
                PRINT( point );

                reload();
                massert(13141, "Chunk map pointed to incorrect chunk", false);
            }
        }

        msgasserted( 8070 ,
                     str::stream() << "couldn't find a chunk which should be impossible: " << point );
    }

    ChunkPtr ChunkManager::findChunkForDoc( const BSONObj& doc ) const {
        BSONObj key = _key.extractKey( doc );
        return findIntersectingChunk( key );
    }

    ChunkPtr ChunkManager::findChunkOnServer( const Shard& shard ) const {
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ) {
            ChunkPtr c = i->second;
            if ( c->getShard() == shard )
                return c;
        }

        return ChunkPtr();
    }

    void ChunkManager::getShardsForQuery( set<Shard>& shards , const BSONObj& query ) const {
        // TODO Determine if the third argument to OrRangeGenerator() is necessary, see SERVER-5165.
        OrRangeGenerator org(_ns.c_str(), query, false);

        const SpecialIndices special = org.getSpecial();
        if (special.has("2d") || special.has("2dsphere")) {
            BSONForEach(field, query) {
                if (getGtLtOp(field) == BSONObj::opNEAR) {
                    uassert(13501, "use geoNear command rather than $near query", false);
                    // TODO: convert to geoNear rather than erroring out
                }
                // $within queries are fine
            }
        } else if (!special.empty()) {
            uassert(13502, "unrecognized special query type: " + special.toString(), false);
        }

        do {
            boost::scoped_ptr<FieldRangeSetPair> frsp (org.topFrsp());

            // special case if most-significant field isn't in query
            FieldRange range = frsp->shardKeyRange(_key.key().firstElementFieldName());
            if ( range.universal() ) {
                DEV PRINT(range.universal());
                getShardsForRange( shards, _key.globalMin(), _key.globalMax() );
                return;
            }
            
            if ( frsp->matchPossibleForSingleKeyFRS( _key.key() ) ) {
                BoundList ranges = _key.keyBounds( frsp->getSingleKeyFRS() );
                for ( BoundList::const_iterator it=ranges.begin(); it != ranges.end(); ++it ){

                    getShardsForRange( shards, it->first /*min*/, it->second /*max*/ );

                    // once we know we need to visit all shards no need to keep looping
                    if( shards.size() == _shards.size() ) return;
                }
            }

            if (!org.orRangesExhausted())
                org.popOrClauseSingleKey();

        }
        while (!org.orRangesExhausted());
        
        // SERVER-4914 Some clients of getShardsForQuery() assume at least one shard will be
        // returned.  For now, we satisfy that assumption by adding a shard with no matches rather
        // than return an empty set of shards.
        if ( shards.empty() ) {
            massert( 16068, "no chunk ranges available", !_chunkRanges.ranges().empty() );
            shards.insert( _chunkRanges.ranges().begin()->second->getShard() );
        }
    }

    void ChunkManager::getShardsForRange( set<Shard>& shards,
                                          const BSONObj& min,
                                          const BSONObj& max ) const {

        ChunkRangeMap::const_iterator it = _chunkRanges.upper_bound(min);
        ChunkRangeMap::const_iterator end = _chunkRanges.upper_bound(max);

        massert( 13507 , str::stream() << "no chunks found between bounds " << min << " and " << max , it != _chunkRanges.ranges().end() );

        if( end != _chunkRanges.ranges().end() ) ++end;

        for( ; it != end; ++it ){
            shards.insert(it->second->getShard());

            // once we know we need to visit all shards no need to keep looping
            if (shards.size() == _shards.size()) break;
        }
    }

    void ChunkManager::getAllShards( set<Shard>& all ) const {
        all.insert(_shards.begin(), _shards.end());
    }

    bool ChunkManager::compatibleWith( const ChunkManager& other, const Shard& shard ) const {
        // Return true if the shard version is the same in the two chunk managers
        // TODO: This doesn't need to be so strong, just major vs
        return other.getVersion( shard ).isEquivalentTo( getVersion( shard ) );

    }

    bool ChunkManager::compatibleWith( const Chunk& other ) const {

        // Do this first, b/c findIntersectingChunk asserts if the key isn't similar
        if( ! this->_key.hasShardKey( other.getMin() ) ) return false;
        // We assume here that chunks will have consistent fields in min/max

        ChunkPtr myChunk = this->findIntersectingChunk( other.getMin() );

        if( other.getMin() != myChunk->getMin() ) return false;
        if( other.getMax() != myChunk->getMax() ) return false;
        if( other.getShard() != myChunk->getShard() ) return false;
        return true;
    }

    void ChunkManager::drop( ChunkManagerPtr me ) const {
        scoped_lock lk( _mutex );

        configServer.logChange( "dropCollection.start" , _ns , BSONObj() );

        DistributedLock nsLock( ConnectionString( configServer.modelServer(),
                                ConnectionString::SYNC ),
                                _ns );

        dist_lock_try dlk;
        try{
        	dlk = dist_lock_try( &nsLock  , "drop" );
        }
        catch( LockException& e ){
        	uassert( 14022, str::stream() << "Error locking distributed lock for chunk drop." << causedBy( e ), false);
        }

        uassert( 13331 ,  "collection's metadata is undergoing changes. Please try again." , dlk.got() );

        uassert( 10174 ,  "config servers not all up" , configServer.allUp() );

        set<Shard> seen;

        LOG(1) << "ChunkManager::drop : " << _ns << endl;

        // lock all shards so no one can do a split/migrate
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ) {
            ChunkPtr c = i->second;
            seen.insert( c->getShard() );
        }

        LOG(1) << "ChunkManager::drop : " << _ns << "\t all locked" << endl;

        map<string,BSONObj> errors;
        // delete data from mongod
        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ) {
            ScopedDbConnection conn(i->getConnString());
            BSONObj info;
            if ( !conn->dropCollection( _ns, &info ) ) {
                errors[ i->getConnString() ] = info;
            }
            conn.done();
        }
        if ( !errors.empty() ) {
            stringstream ss;
            ss << "Dropping collection failed on the following hosts: ";
            for ( map<string,BSONObj>::const_iterator it = errors.begin(); it != errors.end(); ) {
                ss << it->first << ": " << it->second;
                ++it;
                if ( it != errors.end() ) {
                    ss << ", ";
                }
            }
            uasserted( 16338, ss.str() );
        }

        LOG(1) << "ChunkManager::drop : " << _ns << "\t removed shard data" << endl;

        // remove chunk data
        ScopedDbConnection conn(configServer.modelServer());
        conn->remove(ChunkType::ConfigNS, BSON(ChunkType::ns(_ns)));
        
        // Make sure we're dropped on the config
        string error = conn->getLastError();
        uassert( 17001, str::stream() << "could not drop chunks for " << _ns 
                                      << causedBy( error ), 
                 error.size() == 0 );
        
        conn.done();
        LOG(1) << "ChunkManager::drop : " << _ns << "\t removed chunk data" << endl;

        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ) {
            ScopedDbConnection conn(i->getConnString());
            BSONObj res;

            // this is horrible
            // we need a special command for dropping on the d side
            // this hack works for the moment

            if ( ! setShardVersion( conn.conn(),
                                    _ns,
                                    ChunkVersion( 0, OID() ),
                                    ChunkManagerPtr(),
                                    true, res ) )
            {
                throw UserException( 8071 , str::stream() << "cleaning up after drop failed: " << res );
            }

            conn->simpleCommand( "admin", 0, "unsetSharding" );
            conn.done();
        }

        LOG(1) << "ChunkManager::drop : " << _ns << "\t DONE" << endl;
        configServer.logChange( "dropCollection" , _ns , BSONObj() );
    }

    ChunkVersion ChunkManager::getVersion( const Shard& shard ) const {
        ShardVersionMap::const_iterator i = _shardVersions.find( shard );
        if ( i == _shardVersions.end() )
            return ChunkVersion( 0, OID() );
        return i->second;
    }

    ChunkVersion ChunkManager::getVersion() const {
        return _version;
    }

    void ChunkManager::getInfo( BSONObjBuilder& b ) const {
        b.append(CollectionType::keyPattern(), _key.key());
        b.appendBool(CollectionType::unique(), _unique);
        _version.addEpochToBSON(b, CollectionType::DEPRECATED_lastmod());
    }

    string ChunkManager::toString() const {
        stringstream ss;
        ss << "ChunkManager: " << _ns << " key:" << _key.toString() << '\n';
        for ( ChunkMap::const_iterator i=_chunkMap.begin(); i!=_chunkMap.end(); ++i ) {
            const ChunkPtr c = i->second;
            ss << "\t" << c->toString() << '\n';
        }
        return ss.str();
    }

    void ChunkRangeManager::assertValid() const {
        if (_ranges.empty())
            return;

        try {
            // No Nulls
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it) {
                verify(it->second);
            }

            // Check endpoints
            verify(allOfType(MinKey, _ranges.begin()->second->getMin()));
            verify(allOfType(MaxKey, boost::prior(_ranges.end())->second->getMax()));

            // Make sure there are no gaps or overlaps
            for (ChunkRangeMap::const_iterator it=boost::next(_ranges.begin()), end=_ranges.end(); it != end; ++it) {
                ChunkRangeMap::const_iterator last = boost::prior(it);
                verify(it->second->getMin() == last->second->getMax());
            }

            // Check Map keys
            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it) {
                verify(it->first == it->second->getMax());
            }

            // Make sure we match the original chunks
            const ChunkMap chunks = _ranges.begin()->second->getManager()->_chunkMap;
            for ( ChunkMap::const_iterator i=chunks.begin(); i!=chunks.end(); ++i ) {
                const ChunkPtr chunk = i->second;

                ChunkRangeMap::const_iterator min = _ranges.upper_bound(chunk->getMin());
                ChunkRangeMap::const_iterator max = _ranges.lower_bound(chunk->getMax());

                verify(min != _ranges.end());
                verify(max != _ranges.end());
                verify(min == max);
                verify(min->second->getShard() == chunk->getShard());
                verify(min->second->containsPoint( chunk->getMin() ));
                verify(min->second->containsPoint( chunk->getMax() ) || (min->second->getMax() == chunk->getMax()));
            }

        }
        catch (...) {
            error() << "\t invalid ChunkRangeMap! printing ranges:" << endl;

            for (ChunkRangeMap::const_iterator it=_ranges.begin(), end=_ranges.end(); it != end; ++it)
                cout << it->first << ": " << *it->second << endl;

            throw;
        }
    }

    void ChunkRangeManager::reloadAll(const ChunkMap& chunks) {
        _ranges.clear();
        _insertRange(chunks.begin(), chunks.end());

        DEV assertValid();
    }

    void ChunkRangeManager::_insertRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end) {
        while (begin != end) {
            ChunkMap::const_iterator first = begin;
            Shard shard = first->second->getShard();
            while (begin != end && (begin->second->getShard() == shard))
                ++begin;

            shared_ptr<ChunkRange> cr (new ChunkRange(first, begin));
            _ranges[cr->getMax()] = cr;
        }
    }

    int ChunkManager::getCurrentDesiredChunkSize() const {
        // split faster in early chunks helps spread out an initial load better
        const int minChunkSize = 1 << 20;  // 1 MBytes

        int splitThreshold = Chunk::MaxChunkSize;

        int nc = numChunks();

        if ( nc <= 1 ) {
            return 1024;
        }
        else if ( nc < 3 ) {
            return minChunkSize / 2;
        }
        else if ( nc < 10 ) {
            splitThreshold = max( splitThreshold / 4 , minChunkSize );
        }
        else if ( nc < 20 ) {
            splitThreshold = max( splitThreshold / 2 , minChunkSize );
        }

        return splitThreshold;
    }
    
    /** This is for testing only, just setting up minimal basic defaults. */
    ChunkManager::ChunkManager() :
    _unique(),
    _chunkRanges(),
    _mutex( "ChunkManager" ),
    _sequenceNumber()
    {}

    class ChunkObjUnitTest : public StartupTest {
    public:
        void runChunkVersion() {
            vector<ChunkVersion> all;
            all.push_back( ChunkVersion(1,1, OID()) );
            all.push_back( ChunkVersion(1,2, OID()) );
            all.push_back( ChunkVersion(2,1, OID()) );
            all.push_back( ChunkVersion(2,2, OID()) );

            for ( unsigned i=0; i<all.size(); i++ ) {
                for ( unsigned j=i+1; j<all.size(); j++ ) {
                    verify( all[i] < all[j] );
                }
            }

        }

        void run() {
            runChunkVersion();
            LOG(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


    // ----- to be removed ---
    extern OID serverID;

    // NOTE (careful when deprecating)
    //   currently the sharding is enabled because of a write or read (as opposed to a split or migrate), the shard learns
    //   its name and through the 'setShardVersion' command call
    bool setShardVersion( DBClientBase & conn,
                          const string& ns,
                          ChunkVersion version,
                          ChunkManagerPtr manager, // Used only for reporting!
                          bool authoritative ,
                          BSONObj& result )
    {
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append( "setShardVersion" , ns.c_str() );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        version.addToBSON( cmdBuilder );
        cmdBuilder.appendOID( "serverID" , &serverID );
        if ( authoritative )
            cmdBuilder.appendBool( "authoritative" , 1 );

        Shard s = Shard::make( conn.getServerAddress() );
        cmdBuilder.append( "shard" , s.getName() );
        cmdBuilder.append( "shardHost" , s.getConnString() );
        BSONObj cmd = cmdBuilder.obj();

        LOG(1).stream()
               << "    setShardVersion  " << s.getName()
               << " " << conn.getServerAddress()
               << "  " << ns
               << "  " << cmd
               << " " << &conn
               << (manager.get() ? string(str::stream() << " " << manager->getSequenceNumber()) :
                                   "")
               << endl;

        return conn.runCommand("admin", cmd, result, 0);
    }

} // namespace mongo
