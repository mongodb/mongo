// @file chunk.cpp

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

#include "../client/connpool.h"
#include "../db/queryutil.h"
#include "../util/startup_test.h"
#include "../util/timer.h"
#include "mongo/client/dbclientcursor.h"

#include "chunk.h"
#include "config.h"
#include "cursors.h"
#include "grid.h"
#include "strategy.h"
#include "client.h"
#include "mongo/util/concurrency/ticketholder.h"

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

    string Chunk::chunkMetadataNS = "config.chunks";

    int Chunk::MaxChunkSize = 1024 * 1024 * 64;
    int Chunk::MaxObjectPerChunk = 250000;
    

    Chunk::Chunk(const ChunkManager * manager, BSONObj from)
        : _manager(manager), _lastmod(0), _dataWritten(mkDataWritten())
    {
        string ns = from.getStringField( "ns" );
        _shard.reset( from.getStringField( "shard" ) );

        _lastmod = from["lastmod"];
        verify( _lastmod > 0 );

        _min = from.getObjectField( "min" ).getOwned();
        _max = from.getObjectField( "max" ).getOwned();
        
        _jumbo = from["jumbo"].trueValue();

        uassert( 10170 ,  "Chunk needs a ns" , ! ns.empty() );
        uassert( 13327 ,  "Chunk ns must match server ns" , ns == _manager->getns() );

        uassert( 10171 ,  "Chunk needs a server" , _shard.ok() );

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }


    Chunk::Chunk(const ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard)
        : _manager(info), _min(min), _max(max), _shard(shard), _lastmod(0), _jumbo(false), _dataWritten(mkDataWritten())
    {}

    long Chunk::mkDataWritten() {
        return rand() % ( MaxChunkSize / 5 );
    }

    string Chunk::getns() const {
        verify( _manager );
        return _manager->getns();
    }

    bool Chunk::contains( const BSONObj& obj ) const {
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool ChunkRange::contains(const BSONObj& obj) const {
        // same as Chunk method
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    bool Chunk::minIsInf() const {
        return _manager->getShardKey().globalMin().woCompare( getMin() ) == 0;
    }

    bool Chunk::maxIsInf() const {
        return _manager->getShardKey().globalMax().woCompare( getMax() ) == 0;
    }

    BSONObj Chunk::_getExtremeKey( int sort ) const {
        // We need to use a sharded connection here b/c there could be data left from stale migrations outside
        // our chunk ranges.
        ShardConnection conn( getShard().getConnString() , _manager->getns() );
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
        BSONObj end;
        try {
            end = conn->findOne( _manager->getns() , q );
            conn.done();
        }
        catch( StaleConfigException& ){
            // We need to handle stale config exceptions if using sharded connections
            // caught and reported above
            conn.done();
            throw;
        }

        if ( end.isEmpty() )
            return BSONObj();

        return _manager->getShardKey().extractKey( end );
    }

    void Chunk::pickMedianKey( BSONObj& medianKey ) const {
        // Ask the mongod holding this chunk to figure out the split points.
        ScopedDbConnection conn( getShard().getConnString() );
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
        ScopedDbConnection conn( getShard().getConnString() );
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

        // We assume that if the chunk being split is the first (or last) one on the collection, this chunk is
        // likely to see more insertions. Instead of splitting mid-chunk, we use the very first (or last) key
        // as a split point.
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

        ScopedDbConnection conn( getShard().getConnString() );

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

            // reloading won't strictly solve all problems, e.g. the collection's metadata lock can be taken
            // but we issue here so that mongos may refresh without needing to be written/read against
            _manager->reload();

            return false;
        }

        conn.done();
        
        // force reload of config
        _manager->reload();

        return true;
    }

    bool Chunk::moveAndCommit( const Shard& to , long long chunkSize /* bytes */, BSONObj& res ) const {
        uassert( 10167 ,  "can't move shard to its current location!" , getShard() != to );

        log() << "moving chunk ns: " << _manager->getns() << " moving ( " << toString() << ") " << _shard.toString() << " -> " << to.toString() << endl;

        Shard from = _shard;

        ScopedDbConnection fromconn( from);

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
                                                    "configdb" << configServer.modelServer()
                                                ) ,
                                            res
                                          );
        fromconn.done();

        log( worked ) << "moveChunk result: " << res << endl;

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

            if ( _dataWritten < splitThreshold / 5 )
                return false;
            
            if ( ! getManager()->_splitTickets.tryAcquire() ) {
                LOG(1) << "won't auto split becaue not enough tickets: " << getManager()->getns() << endl;
                return false;
            }
            TicketHolderReleaser releaser( &getManager()->_splitTickets );

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
                ChunkPtr toMove = cm->findChunk(min);

                if ( ! (toMove->getMin() == min && toMove->getMax() == max) ){
                    LOG(1) << "recently split chunk: " << range << " modified before we could migrate " << toMove << endl;
                    return true;
                }

                log() << "moving chunk (auto): " << toMove << " to: " << newLocation.toString() << endl;

                BSONObj res;
                massert( 10412 ,
                         str::stream() << "moveAndCommit failed: " << res ,
                         toMove->moveAndCommit( newLocation , MaxChunkSize , res ) );
                
                // update our config
                _manager->reload();
            }

            return true;

        }
        catch ( DBException& e ) {
            // if the collection lock is taken (e.g. we're migrating), it is fine for the split to fail.
            warning() << "could not autosplit collection " << _manager->getns() << causedBy( e ) << endl;
            return false;
        }
    }

    long Chunk::getPhysicalSize() const {
        ScopedDbConnection conn( getShard().getConnString() );

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
        bb.append( "min" , _min );
        bb.append( "max" , _max );
        bb.done();
    }

    bool Chunk::operator==( const Chunk& s ) const {
        return
            _manager->getShardKey().compare( _min , s._min ) == 0 &&
            _manager->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    void Chunk::serialize(BSONObjBuilder& to,ShardChunkVersion myLastMod) {

        to.append( "_id" , genID( _manager->getns() , _min ) );

        if ( myLastMod.isSet() ) {
            to.appendTimestamp( "lastmod" , myLastMod );
        }
        else if ( _lastmod.isSet() ) {
            verify( _lastmod > 0 && _lastmod < 1000 );
            to.appendTimestamp( "lastmod" , _lastmod );
        }
        else {
            verify(0);
        }

        to << "ns" << _manager->getns();
        to << "min" << _min;
        to << "max" << _max;
        to << "shard" << _shard.getName();
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
        ss << "ns:" << _manager->getns() << " at: " << _shard.toString() << " lastmod: " << _lastmod.toString() << " min: " << _min << " max: " << _max;
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
            ScopedDbConnection conn( configServer.modelServer() );
            conn->update( chunkMetadataNS , BSON( "_id" << genID() ) , BSON( "$set" << BSON( "jumbo" << true ) ) );
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

        int csize = o["value"].numberInt();

        // validate chunksize before proceeding
        if ( csize == 0 ) {
            // setting was not modified; mark as such
            log() << "warning: invalid chunksize (" << csize << ") ignored" << endl;
            return;
        }

        LOG(1) << "Refreshing MaxChunkSize: " << csize << endl;
        Chunk::MaxChunkSize = csize * 1024 * 1024;
    }

    // -------  ChunkManager --------

    AtomicUInt ChunkManager::NextSequenceNumber = 1;

    ChunkManager::ChunkManager( string ns , ShardKeyPattern pattern , bool unique ) :
        _ns( ns ) , _key( pattern ) , _unique( unique ) , _chunkRanges(), _mutex("ChunkManager"),
        _nsLock( ConnectionString( configServer.modelServer() , ConnectionString::SYNC ) , ns ),

        // The shard versioning mechanism hinges on keeping track of the number of times we reloaded ChunkManager's.
        // Increasing this number here will prompt checkShardVersion() to refresh the connection-level versions to
        // the most up to date value.
        _sequenceNumber(++NextSequenceNumber),

        _splitTickets( 5 )

    {
        int tries = 3;
        while (tries--) {
            ChunkMap chunkMap;
            set<Shard> shards;
            ShardVersionMap shardVersions;
            Timer t;
            _load(chunkMap, shards, shardVersions);
            {
                int ms = t.millis();
                log() << "ChunkManager: time to load chunks for " << ns << ": " << ms << "ms" 
                      << " sequenceNumber: " << _sequenceNumber 
                      << " version: " << _version.toString() 
                      << endl;
            }

            if (_isValid(chunkMap)) {
                // These variables are const for thread-safety. Since the
                // constructor can only be called from one thread, we don't have
                // to worry about that here.
                const_cast<ChunkMap&>(_chunkMap).swap(chunkMap);
                const_cast<set<Shard>&>(_shards).swap(shards);
                const_cast<ShardVersionMap&>(_shardVersions).swap(shardVersions);
                const_cast<ChunkRangeManager&>(_chunkRanges).reloadAll(_chunkMap);
                return;
            }
            
            if (_chunkMap.size() < 10) {
                _printChunks();
            }
            
            warning() << "ChunkManager loaded an invalid config, trying again" << endl;

            sleepmillis(10 * (3-tries));
        }

        // this will abort construction so we should never have a reference to an invalid config
        msgasserted(13282, "Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.");
    }

    ChunkManagerPtr ChunkManager::reload(bool force) const {
        return grid.getDBConfig(getns())->getChunkManager(getns(), force);
    }

    void ChunkManager::_load(ChunkMap& chunkMap, set<Shard>& shards, ShardVersionMap& shardVersions) {
        ScopedDbConnection conn( configServer.modelServer() );

        // TODO really need the sort?
        auto_ptr<DBClientCursor> cursor = conn->query( Chunk::chunkMetadataNS, QUERY("ns" << _ns).sort("lastmod",-1), 0, 0, 0, 0,
                                          (DEBUG_BUILD ? 2 : 1000000)); // batch size. Try to induce potential race conditions in debug builds
        verify( cursor.get() );
        while ( cursor->more() ) {
            BSONObj d = cursor->next();
            if ( d["isMaxMarker"].trueValue() ) {
                continue;
            }

            ChunkPtr c( new Chunk( this, d ) );

            chunkMap[c->getMax()] = c;
            shards.insert(c->getShard());
            

            // set global max
            if ( c->getLastmod() > _version )
                _version = c->getLastmod();
            
            // set shard max
            ShardChunkVersion& shardMax = shardVersions[c->getShard()];
            if ( c->getLastmod() > shardMax )
                shardMax = c->getLastmod();
        }
        conn.done();
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

    void ChunkManager::createFirstChunks( const Shard& primary , vector<BSONObj>* initPoints , vector<Shard>* initShards ) const {
        // TODO distlock?
        verify( _chunkMap.size() == 0 );

        vector<BSONObj> splitPoints;
        vector<Shard> shards;
        unsigned long long numObjects = 0;
        Chunk c(this, _key.globalMin(), _key.globalMax(), primary);

        if ( !initPoints || !initPoints->size() ) {
            // discover split points
            {
                // get stats to see if there is any data
                ScopedDbConnection shardConn( primary.getConnString() );
                numObjects = shardConn->count( getns() );
                shardConn.done();
            }

            if ( numObjects > 0 )
                c.pickSplitVector( splitPoints , Chunk::MaxChunkSize );

            // since docs alread exists, must use primary shard
            shards.push_back( primary );
        } else {
            // make sure points are unique and ordered
            set<BSONObj> orderedPts;
            for ( unsigned i = 0; i < initPoints->size(); ++i ) {
                BSONObj pt = (*initPoints)[i];
                orderedPts.insert( pt );
            }
            for ( set<BSONObj>::iterator it = orderedPts.begin(); it != orderedPts.end(); ++it ) {
                splitPoints.push_back( *it );
            }

            if ( !initShards || !initShards->size() ) {
                // use all shards, starting with primary
                shards.push_back( primary );
                vector<Shard> tmp;
                primary.getAllShards( tmp );
                for ( unsigned i = 0; i < tmp.size(); ++i ) {
                    if ( tmp[i] != primary )
                        shards.push_back( tmp[i] );
                }
            }
        }

        // this is the first chunk; start the versioning from scratch
        ShardChunkVersion version;
        version.incMajor();
        
        log() << "going to create " << splitPoints.size() + 1 << " chunk(s) for: " << _ns << endl;
        
        ScopedDbConnection conn( configServer.modelServer() );        

        for ( unsigned i=0; i<=splitPoints.size(); i++ ) {
            BSONObj min = i == 0 ? _key.globalMin() : splitPoints[i-1];
            BSONObj max = i < splitPoints.size() ? splitPoints[i] : _key.globalMax();
            
            Chunk temp( this , min , max , shards[ i % shards.size() ] );
        
            BSONObjBuilder chunkBuilder;
            temp.serialize( chunkBuilder , version );
            BSONObj chunkObj = chunkBuilder.obj();
        
            conn->update( Chunk::chunkMetadataNS, QUERY( "_id" << temp.genID() ), chunkObj,  true, false );

            version.incMinor();
        }

        string errmsg = conn->getLastError();
        if ( errmsg.size() ) {
            string ss = str::stream() << "creating first chunks failed. result: " << errmsg;
            error() << ss << endl;
            msgasserted( 15903 , ss );
        }
        
        conn.done();

        if ( numObjects == 0 ) {
            // the ensure index will have the (desired) indirect effect of creating the collection on the
            // assigned shard, as it sets up the index over the sharding keys.
            ScopedDbConnection shardConn( c.getShard().getConnString() );
            shardConn->ensureIndex( getns() , getShardKey().key() , _unique , "" , false ); // do not cache ensureIndex SERVER-1691 
            shardConn.done();
        }

    }

    ChunkPtr ChunkManager::findChunk( const BSONObj & obj ) const {
        BSONObj key = _key.extractKey(obj);

        {
            BSONObj foo;
            ChunkPtr c;
            {
                ChunkMap::const_iterator it = _chunkMap.upper_bound(key);
                if (it != _chunkMap.end()) {
                    foo = it->first;
                    c = it->second;
                }
            }

            if ( c ) {
                if ( c->contains( key ) ){
                    dassert(c->contains(key)); // doesn't use fast-path in extractKey
                    return c;
                }

                PRINT(foo);
                PRINT(*c);
                PRINT(key);

                reload();
                massert(13141, "Chunk map pointed to incorrect chunk", false);
            }
        }

        throw UserException( 8070 , str::stream() << "couldn't find a chunk which should be impossible: " << key );
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

        const string special = org.getSpecial();
        if (special == "2d") {
            BSONForEach(field, query) {
                if (getGtLtOp(field) == BSONObj::opNEAR) {
                    uassert(13501, "use geoNear command rather than $near query", false);
                    // TODO: convert to geoNear rather than erroring out
                }
                // $within queries are fine
            }
        }
        else if (!special.empty()) {
            uassert(13502, "unrecognized special query type: " + special, false);
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
            
            if ( frsp->matchPossibleForShardKey( _key.key() ) ) {
                BoundList ranges = frsp->shardKeyIndexBounds(_key.key());
                for (BoundList::const_iterator it=ranges.begin(), end=ranges.end();
                     it != end; ++it) {

                    BSONObj minObj = it->first.replaceFieldNames(_key.key());
                    BSONObj maxObj = it->second.replaceFieldNames(_key.key());

                    getShardsForRange( shards, minObj, maxObj, false );

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

    void ChunkManager::getShardsForRange(set<Shard>& shards, const BSONObj& min, const BSONObj& max, bool fullKeyReq ) const {

        if( fullKeyReq ){
            uassert(13405, str::stream() << "min value " << min << " does not have shard key", hasShardKey(min));
            uassert(13406, str::stream() << "max value " << max << " does not have shard key", hasShardKey(max));
        }

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
        return other.getVersion( shard ).toLong() == getVersion( shard ).toLong();

    }

    bool ChunkManager::compatibleWith( const Chunk& other ) const {
        ChunkPtr myChunk = this->findChunk( other.getMin() );

        if( other.getMin() != myChunk->getMin() ) return false;
        if( other.getMax() != myChunk->getMax() ) return false;
        if( other.getShard() != myChunk->getShard() ) return false;
        return true;
    }

    void ChunkManager::drop( ChunkManagerPtr me ) const {
        scoped_lock lk( _mutex );

        configServer.logChange( "dropCollection.start" , _ns , BSONObj() );

        dist_lock_try dlk;
        try{
        	dlk = dist_lock_try( &_nsLock  , "drop" );
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

        // delete data from mongod
        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ) {
            ScopedDbConnection conn( *i );
            conn->dropCollection( _ns );
            conn.done();
        }

        LOG(1) << "ChunkManager::drop : " << _ns << "\t removed shard data" << endl;

        // remove chunk data
        ScopedDbConnection conn( configServer.modelServer() );
        conn->remove( Chunk::chunkMetadataNS , BSON( "ns" << _ns ) );
        conn.done();
        LOG(1) << "ChunkManager::drop : " << _ns << "\t removed chunk data" << endl;

        for ( set<Shard>::iterator i=seen.begin(); i!=seen.end(); i++ ) {
            ScopedDbConnection conn( *i );
            BSONObj res;

            // this is horrible
            // we need a special command for dropping on the d side
            // this hack works for the moment

            if ( ! setShardVersion( conn.conn() , _ns , 0 , true , res ) )
                throw UserException( 8071 , str::stream() << "cleaning up after drop failed: " << res );
            conn->simpleCommand( "admin", 0, "unsetSharding" );
            conn.done();
        }

        LOG(1) << "ChunkManager::drop : " << _ns << "\t DONE" << endl;
        configServer.logChange( "dropCollection" , _ns , BSONObj() );
    }

    ShardChunkVersion ChunkManager::getVersion( const Shard& shard ) const {
        ShardVersionMap::const_iterator i = _shardVersions.find( shard );
        if ( i == _shardVersions.end() )
            return 0;
        return i->second;
    }

    ShardChunkVersion ChunkManager::getVersion() const {
        return _version;
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
                verify(min->second->contains( chunk->getMin() ));
                verify(min->second->contains( chunk->getMax() ) || (min->second->getMax() == chunk->getMax()));
            }

        }
        catch (...) {
            log( LL_ERROR ) << "\t invalid ChunkRangeMap! printing ranges:" << endl;

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
    _nsLock( ConnectionString(), "" ),
    _sequenceNumber(),
    _splitTickets( 0 ){
    }

    class ChunkObjUnitTest : public StartupTest {
    public:
        void runShardChunkVersion() {
            vector<ShardChunkVersion> all;
            all.push_back( ShardChunkVersion(1,1) );
            all.push_back( ShardChunkVersion(1,2) );
            all.push_back( ShardChunkVersion(2,1) );
            all.push_back( ShardChunkVersion(2,2) );

            for ( unsigned i=0; i<all.size(); i++ ) {
                for ( unsigned j=i+1; j<all.size(); j++ ) {
                    verify( all[i] < all[j] );
                }
            }

        }

        void run() {
            runShardChunkVersion();
            LOG(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


    // ----- to be removed ---
    extern OID serverID;

    // NOTE (careful when deprecating)
    //   currently the sharding is enabled because of a write or read (as opposed to a split or migrate), the shard learns
    //   its name and through the 'setShardVersion' command call
    bool setShardVersion( DBClientBase & conn , const string& ns , ShardChunkVersion version , bool authoritative , BSONObj& result ) {
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append( "setShardVersion" , ns.c_str() );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        cmdBuilder.appendTimestamp( "version" , version.toLong() );
        cmdBuilder.appendOID( "serverID" , &serverID );
        if ( authoritative )
            cmdBuilder.appendBool( "authoritative" , 1 );

        Shard s = Shard::make( conn.getServerAddress() );
        cmdBuilder.append( "shard" , s.getName() );
        cmdBuilder.append( "shardHost" , s.getConnString() );
        BSONObj cmd = cmdBuilder.obj();

        LOG(1) << "    setShardVersion  " << s.getName() << " " << conn.getServerAddress() << "  " << ns << "  " << cmd << " " << &conn << endl;

        return conn.runCommand( "admin" , cmd , result );
    }

} // namespace mongo
