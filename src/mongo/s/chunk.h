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
#include "../client/distlock.h"

#include "shardkey.h"
#include "shard.h"
#include "util.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

    class DBConfig;
    class Chunk;
    class ChunkRange;
    class ChunkManager;
    class ChunkObjUnitTest;

    typedef shared_ptr<const Chunk> ChunkPtr;

    // key is max for each Chunk or ChunkRange
    typedef map<BSONObj,ChunkPtr,BSONObjCmp> ChunkMap;
    typedef map<BSONObj,shared_ptr<ChunkRange>,BSONObjCmp> ChunkRangeMap;

    typedef shared_ptr<const ChunkManager> ChunkManagerPtr;

    /**
       config.chunks
       { ns : "alleyinsider.fs.chunks" , min : {} , max : {} , server : "localhost:30001" }

       x is in a shard iff
       min <= x < max
     */
    class Chunk : boost::noncopyable {
    public:
        Chunk( const ChunkManager * info , BSONObj from);
        Chunk( const ChunkManager * info , const BSONObj& min, const BSONObj& max, const Shard& shard);

        //
        // serialization support
        //

        void serialize(BSONObjBuilder& to, ShardChunkVersion myLastMod=0);

        //
        // chunk boundary support
        //

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }

        // if min/max key is pos/neg infinity
        bool minIsInf() const;
        bool maxIsInf() const;

        bool contains( const BSONObj& obj ) const;

        string genID() const;
        static string genID( const string& ns , const BSONObj& min );

        //
        // chunk version support
        //

        void appendShortVersion( const char * name , BSONObjBuilder& b ) const;

        ShardChunkVersion getLastmod() const { return _lastmod; }
        void setLastmod( ShardChunkVersion v ) { _lastmod = v; }

        //
        // split support
        //

        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         * @return if something was split
         */
        bool splitIfShould( long dataWritten ) const;

        /**
         * Splits this chunk at a non-specificed split key to be chosen by the mongod holding this chunk.
         *
         * @param force if set to true, will split the chunk regardless if the split is really necessary size wise
         *              if set to false, will only split if the chunk has reached the currently desired maximum size
         * @param res the object containing details about the split execution
         * @return splitPoint if found a key and split successfully, else empty BSONObj
         */
        BSONObj singleSplit( bool force , BSONObj& res ) const;

        /**
         * Splits this chunk at the given key (or keys)
         *
         * @param splitPoints the vector of keys that should be used to divide this chunk
         * @param res the object containing details about the split execution
         * @return if the split was successful
         */
        bool multiSplit( const  vector<BSONObj>& splitPoints , BSONObj& res ) const;

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
         * Issues a migrate request for this chunk
         *
         * @param to shard to move this chunk to
         * @param chunSize maximum number of bytes beyond which the migrate should no go trhough
         * @param res the object containing details about the migrate execution
         * @return true if move was successful
         */
        bool moveAndCommit( const Shard& to , long long chunkSize , BSONObj& res ) const;

        /**
         * @return size of shard in bytes
         *  talks to mongod to do this
         */
        long getPhysicalSize() const;

        /**
         * marks this chunk as a jumbo chunk
         * that means the chunk will be inelligble for migrates
         */
        void markAsJumbo() const;

        bool isJumbo() const { return _jumbo; }

        /**
         * Attempt to refresh maximum chunk size from config.
         */
         static void refreshChunkSize();

        //
        // public constants
        //

        static string chunkMetadataNS;
        static int MaxChunkSize;
        static int MaxObjectPerChunk;
        //
        // accessors and helpers
        //

        string toString() const;

        friend ostream& operator << (ostream& out, const Chunk& c) { return (out << c.toString()); }
        bool operator==(const Chunk& s) const;
        bool operator!=(const Chunk& s) const { return ! ( *this == s ); }

        string getns() const;
        const char * getNS() { return "config.chunks"; }
        Shard getShard() const { return _shard; }
        const ChunkManager* getManager() const { return _manager; }
        

    private:

        // main shard info
        
        const ChunkManager * _manager;

        BSONObj _min;
        BSONObj _max;
        Shard _shard;
        ShardChunkVersion _lastmod;
        mutable bool _jumbo;

        // transient stuff

        mutable long _dataWritten;

        // methods, etc..

        /**
         * if sort 1, return lowest key
         * if sort -1, return highest key
         * will return empty object if have none
         */
        BSONObj _getExtremeKey( int sort ) const;

        /** initializes _dataWritten with a random value so that a mongos restart wouldn't cause delay in splitting */
        static long mkDataWritten();

        ShardKeyPattern skey() const;
    };

    class ChunkRange {
    public:
        const ChunkManager* getManager() const { return _manager; }
        Shard getShard() const { return _shard; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }

        // clones of Chunk methods
        bool contains(const BSONObj& obj) const;

        ChunkRange(ChunkMap::const_iterator begin, const ChunkMap::const_iterator end)
            : _manager(begin->second->getManager())
            , _shard(begin->second->getShard())
            , _min(begin->second->getMin())
            , _max(boost::prior(end)->second->getMax()) {
            verify( begin != end );

            DEV while (begin != end) {
                verify(begin->second->getManager() == _manager);
                verify(begin->second->getShard() == _shard);
                ++begin;
            }
        }

        // Merge min and max (must be adjacent ranges)
        ChunkRange(const ChunkRange& min, const ChunkRange& max)
            : _manager(min.getManager())
            , _shard(min.getShard())
            , _min(min.getMin())
            , _max(max.getMax()) {
            verify(min.getShard() == max.getShard());
            verify(min.getManager() == max.getManager());
            verify(min.getMax() == max.getMin());
        }

        friend ostream& operator<<(ostream& out, const ChunkRange& cr) {
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
        typedef map<Shard,ShardChunkVersion> ShardVersionMap;

        ChunkManager( string ns , ShardKeyPattern pattern , bool unique );

        string getns() const { return _ns; }

        int numChunks() const { return _chunkMap.size(); }
        bool hasShardKey( const BSONObj& obj ) const;

        void createFirstChunks( const Shard& primary , vector<BSONObj>* initPoints , vector<Shard>* initShards ) const; // only call from DBConfig::shardCollection
        ChunkPtr findChunk( const BSONObj& obj ) const;
        ChunkPtr findChunkOnServer( const Shard& shard ) const;

        const ShardKeyPattern& getShardKey() const {  return _key; }
        bool isUnique() const { return _unique; }

        void getShardsForQuery( set<Shard>& shards , const BSONObj& query ) const;
        void getAllShards( set<Shard>& all ) const;
        /** @param shards set to the shards covered by the interval [min, max], see SERVER-4791 */
        void getShardsForRange(set<Shard>& shards, const BSONObj& min, const BSONObj& max, bool fullKeyReq = true) const;

        ChunkMap getChunkMap() const { return _chunkMap; }

        /**
         * Returns true if, for this shard, the chunks are identical in both chunk managers
         */
        bool compatibleWith( const ChunkManager& other, const Shard& shard ) const;
        bool compatibleWith( ChunkManagerPtr other, const Shard& shard ) const { if( ! other ) return false; return compatibleWith( *other, shard ); }

        bool compatibleWith( const Chunk& other ) const;
        bool compatibleWith( ChunkPtr other ) const { if( ! other ) return false; return compatibleWith( *other ); }



        string toString() const;

        ShardChunkVersion getVersion( const Shard& shard ) const;
        ShardChunkVersion getVersion() const;

        /**
         * this is just an increasing number of how many ChunkManagers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber() const { return _sequenceNumber; }

        void getInfo( BSONObjBuilder& b ) const {
            b.append( "key" , _key.key() );
            b.appendBool( "unique" , _unique );
        }

        /**
         * @param me - so i don't get deleted before i'm done
         */
        void drop( ChunkManagerPtr me ) const;

        void _printChunks() const;

        int getCurrentDesiredChunkSize() const;

    private:
        ChunkManagerPtr reload(bool force=true) const; // doesn't modify self!

        // helpers for constructor
        void _load(ChunkMap& chunks, set<Shard>& shards, ShardVersionMap& shardVersions);
        static bool _isValid(const ChunkMap& chunks);

        // All members should be const for thread-safety
        const string _ns;
        const ShardKeyPattern _key;
        const bool _unique;

        const ChunkMap _chunkMap;
        const ChunkRangeManager _chunkRanges;

        const set<Shard> _shards;

        const ShardVersionMap _shardVersions; // max version per shard

        ShardChunkVersion _version; // max version of any chunk

        mutable mutex _mutex; // only used with _nsLock
        mutable DistributedLock _nsLock;

        const unsigned long long _sequenceNumber;

        mutable TicketHolder _splitTickets; // number of concurrent splitVector we can do from a splitIfShould per collection
        
        friend class Chunk;
        friend class ChunkRangeManager; // only needed for CRM::assertValid()
        static AtomicUInt NextSequenceNumber;
        
        /** Just for testing */
        friend class TestableChunkManager;
        ChunkManager();
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
