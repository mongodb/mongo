// @file chunk.h

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

#pragma once

#include "../pch.h"

#include "../bson/util/atomic_int.h"
#include "../client/dbclient.h"
#include "../client/distlock.h"

#include "shardkey.h"
#include "shard.h"
#include "util.h"

namespace mongo {
    
    class DBConfig;
    class Chunk;
    class ChunkRange;
    class ChunkManager;
    class ChunkRangeMangager;
    class ChunkObjUnitTest;

    typedef shared_ptr<Chunk> ChunkPtr;

    // key is max for each Chunk or ChunkRange
    typedef map<BSONObj,ChunkPtr,BSONObjCmp> ChunkMap;
    typedef map<BSONObj,shared_ptr<ChunkRange>,BSONObjCmp> ChunkRangeMap;
    
    typedef shared_ptr<ChunkManager> ChunkManagerPtr;

    /**
       config.chunks
       { ns : "alleyinsider.fs.chunks" , min : {} , max : {} , server : "localhost:30001" }
       
       x is in a shard iff
       min <= x < max
     */    
    class Chunk : boost::noncopyable, public boost::enable_shared_from_this<Chunk>  {
    public:
        Chunk( ChunkManager * info );
        Chunk( ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard);

        //
        // serialization support
        //

        void serialize(BSONObjBuilder& to, ShardChunkVersion myLastMod=0);
        void unserialize(const BSONObj& from);
        
        //
        // chunk boundary support
        //

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        void setMin(const BSONObj& o) { _min = o; }
        void setMax(const BSONObj& o) { _max = o; }

        // if min/max key is pos/neg infinity
        bool minIsInf() const;
        bool maxIsInf() const;

        bool contains( const BSONObj& obj ) const;

        string genID() const;
        static string genID( const string& ns , const BSONObj& min );

        //
        // chunk version support
        // 

        void appendShortVersion( const char * name , BSONObjBuilder& b );

        ShardChunkVersion getLastmod() const { return _lastmod; }
        void setLastmod( ShardChunkVersion v ) { _lastmod = v; }

        //
        // split support
        //

        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         */
        bool splitIfShould( long dataWritten );
        
        /**
         * Splits this chunk at a non-specificed split key to be chosen by the mongod holding this chunk.
         *
         * @param force if set to true, will split the chunk regardless if the split is really necessary size wise
         *              if set to false, will only split if the chunk has reached the currently desired maximum size
         * @return if found a key, return a pointer to the first chunk, otherwise return a null pointer
         */
        ChunkPtr singleSplit( bool force );

        /**
         * Splits this chunk at the given key (or keys)
         *
         * @param splitPoints the vector of keys that should be used to divide this chunk
         * @return shared pointer to the first new Chunk
         */
        ChunkPtr multiSplit( const  vector<BSONObj>& splitPoints );

        /**
         * Asks the mongod holding this chunk to find a key that approximately divides this chunk in two
         * 
         * @param medianKey the key that divides this chunk, if there is one, or empty
         */
        void pickMedianKey( BSONObj& medianKey ) const;

        /**
         * @param splitPoints vector to be filled in
         * @param chunkSize chunk size to target in bytes
         * @param maxPoints limits the number of split points that are needed, zero is max (optional)
         * @param maxObjs limits the number of objects in each chunk, zero is as max (optional)
         */
        void pickSplitVector( vector<BSONObj>& splitPoints , int chunkSize , int maxPoints = 0, int maxObjs = 0) const;

        //
        // migration support
        // 

        /**
         * moves either this shard or newShard if it makes sense too
         *
         * @return whether or not a shard was moved
         */
        bool moveIfShould( ChunkPtr newShard = ChunkPtr() );

        /**
         * Issues a migrate request for this chunk
         *
         * @param to shard to move this chunk to
         * @param chunSize maximum number of bytes beyond which the migrate should no go trhough
         * @param res the object containing details about the migrate execution
         * @return true if move was successful
         */
        bool moveAndCommit( const Shard& to , long long chunkSize , BSONObj& res );

        /**
         * @return size of shard in bytes
         *  talks to mongod to do this
         */
        long getPhysicalSize() const;

        //
        // chunk size support
        
        int countObjects(int maxcount=0) const;
        
        //
        // public constants 
        //

        static string chunkMetadataNS;    
        static int MaxChunkSize;

        //
        // accessors and helpers
        //

        string toString() const;

        friend ostream& operator << (ostream& out, const Chunk& c){ return (out << c.toString()); }
        bool operator==(const Chunk& s) const;
        bool operator!=(const Chunk& s) const { return ! ( *this == s ); }
        
        string getns() const;
        const char * getNS() { return "config.chunks"; }
        Shard getShard() const { return _shard; }
        const ChunkManager* getManager() const { return _manager; }

    private:
        // main shard info
        
        ChunkManager * _manager;

        BSONObj _min;
        BSONObj _max;
        Shard _shard;
        ShardChunkVersion _lastmod;

        // transient stuff

        long _dataWritten;

        // methods, etc..
        
        /**
         * if sort 1, return lowest key
         * if sort -1, return highest key
         * will return empty object if have none
         */
        BSONObj _getExtremeKey( int sort ) const;

        /** initializes _dataWritten with a random value so that a mongos restart wouldn't cause delay in splitting */
        void _setDataWritten();

        ShardKeyPattern skey() const;
    };

    class ChunkRange{
    public:
        const ChunkManager* getManager() const{ return _manager; }
        Shard getShard() const{ return _shard; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }

        // clones of Chunk methods
        bool contains(const BSONObj& obj) const;

        ChunkRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end)
            : _manager(begin->second->getManager())
            , _shard(begin->second->getShard())
            , _min(begin->second->getMin())
            , _max(prior(end)->second->getMax())
        {
            assert( begin != end );

            DEV while (begin != end){
                assert(begin->second->getManager() == _manager);
                assert(begin->second->getShard() == _shard);
                ++begin;
            }
        }

        // Merge min and max (must be adjacent ranges)
        ChunkRange(const ChunkRange& min, const ChunkRange& max)
            : _manager(min.getManager())
            , _shard(min.getShard())
            , _min(min.getMin())
            , _max(max.getMax())
        {
            assert(min.getShard() == max.getShard());
            assert(min.getManager() == max.getManager());
            assert(min.getMax() == max.getMin());
        }

        friend ostream& operator<<(ostream& out, const ChunkRange& cr){
            return (out << "ChunkRange(min=" << cr._min << ", max=" << cr._max << ", shard=" << cr._shard <<")");
        }

    private:
        const ChunkManager* _manager;
        const Shard _shard;
        const BSONObj _min;
        const BSONObj _max;
    };


    class ChunkRangeManager {
    public:
        const ChunkRangeMap& ranges() const { return _ranges; }

        void clear() { _ranges.clear(); }

        void reloadAll(const ChunkMap& chunks);
        void reloadRange(const ChunkMap& chunks, const BSONObj& min, const BSONObj& max);

        // Slow operation -- wrap with DEV
        void assertValid() const;

        ChunkRangeMap::const_iterator upper_bound(const BSONObj& o) const { return _ranges.upper_bound(o); }
        ChunkRangeMap::const_iterator lower_bound(const BSONObj& o) const { return _ranges.lower_bound(o); }

    private:
        // assumes nothing in this range exists in _ranges
        void _insertRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end);

        ChunkRangeMap _ranges;
    };

    /* config.sharding
         { ns: 'alleyinsider.fs.chunks' , 
           key: { ts : 1 } ,
           shards: [ { min: 1, max: 100, server: a } , { min: 101, max: 200 , server : b } ]
         }
    */
    class ChunkManager {
    public:

        ChunkManager( string ns , ShardKeyPattern pattern , bool unique );
        virtual ~ChunkManager();

        string getns() const { return _ns; }
        
        int numChunks() const { rwlock lk( _lock , false ); return _chunkMap.size(); }
        bool hasShardKey( const BSONObj& obj );

        void createFirstChunk( const Shard& shard );
        ChunkPtr findChunk( const BSONObj& obj , bool retry = false );
        ChunkPtr findChunkOnServer( const Shard& shard ) const;
        
        const ShardKeyPattern& getShardKey() const {  return _key; }
        bool isUnique() const { return _unique; }

        void maybeChunkCollection();
        
        void getShardsForQuery( set<Shard>& shards , const BSONObj& query );
        void getAllShards( set<Shard>& all );
        void getShardsForRange(set<Shard>& shards, const BSONObj& min, const BSONObj& max); // [min, max)

        string toString() const;

        ShardChunkVersion getVersion( const Shard& shard ) const;
        ShardChunkVersion getVersion() const;

        /**
         * this is just an increasing number of how many ChunkManagers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber() const { return _sequenceNumber; }
        
        void getInfo( BSONObjBuilder& b ){
            b.append( "key" , _key.key() );
            b.appendBool( "unique" , _unique );
        }
        
        /**
         * @param me - so i don't get deleted before i'm done
         */
        void drop( ChunkManagerPtr me );

        void _printChunks() const;
        
        int getCurrentDesiredChunkSize() const;

    private:        
        void _reload();
        void _reload_inlock();
        void _load();

        void ensureIndex_inlock();
        
        string _ns;
        ShardKeyPattern _key;
        bool _unique;
        
        ChunkMap _chunkMap;
        ChunkRangeManager _chunkRanges;

        set<Shard> _shards;

        unsigned long long _sequenceNumber;
        
        mutable RWLock _lock;
        DistributedLock _nsLock;

        friend class Chunk;
        friend class ChunkRangeManager; // only needed for CRM::assertValid()
        static AtomicUInt NextSequenceNumber;

        bool _isValid() const;
    };

    // like BSONObjCmp. for use as an STL comparison functor
    // key-order in "order" argument must match key-order in shardkey
    class ChunkCmp {
    public:
        ChunkCmp( const BSONObj &order = BSONObj() ) : _cmp( order ) {}
        bool operator()( const Chunk &l, const Chunk &r ) const {
            return _cmp(l.getMin(), r.getMin());
        }
        bool operator()( const ptr<Chunk> l, const ptr<Chunk> r ) const {
            return operator()(*l, *r);
        }

        // Also support ChunkRanges
        bool operator()( const ChunkRange &l, const ChunkRange &r ) const {
            return _cmp(l.getMin(), r.getMin());
        }
        bool operator()( const shared_ptr<ChunkRange> l, const shared_ptr<ChunkRange> r ) const {
            return operator()(*l, *r);
        }
    private:
        BSONObjCmp _cmp;
    };

    /*
    struct chunk_lock {
        chunk_lock( const Chunk* c ){
            
        }
        
        Chunk _c;
    };
    */
    inline string Chunk::genID() const { return genID(_manager->getns(), _min); }

    bool setShardVersion( DBClientBase & conn , const string& ns , ShardChunkVersion version , bool authoritative , BSONObj& result );

} // namespace mongo
