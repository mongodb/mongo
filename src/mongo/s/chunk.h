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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/util/atomic_int.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/distlock.h"
#include "mongo/s/shard.h"
#include "mongo/s/shardkey.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    class DBConfig;
    class Chunk;
    class ChunkRange;
    class ChunkManager;
    class ChunkObjUnitTest;

    typedef shared_ptr<const Chunk> ChunkPtr;

    // key is max for each Chunk or ChunkRange
    typedef std::map<BSONObj,ChunkPtr,BSONObjCmp> ChunkMap;
    typedef std::map<BSONObj,shared_ptr<ChunkRange>,BSONObjCmp> ChunkRangeMap;

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
        Chunk( const ChunkManager * info ,
               const BSONObj& min,
               const BSONObj& max,
               const Shard& shard,
               ChunkVersion lastmod = ChunkVersion() );

        //
        // serialization support
        //

        void serialize(BSONObjBuilder& to, ChunkVersion myLastMod=ChunkVersion(0,OID()));

        //
        // chunk boundary support
        //

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }

        // if min/max key is pos/neg infinity
        bool minIsInf() const;
        bool maxIsInf() const;

        // Returns true if this chunk contains the given point, and false otherwise
        //
        // Note: this function takes an extracted *key*, not an original document
        // (the point may be computed by, say, hashing a given field or projecting
        //  to a subset of fields).
        bool containsPoint( const BSONObj& point ) const;

        std::string genID() const;
        static std::string genID( const std::string& ns , const BSONObj& min );

        //
        // chunk version support
        //

        void appendShortVersion( const char * name , BSONObjBuilder& b ) const;

        ChunkVersion getLastmod() const { return _lastmod; }
        void setLastmod( ChunkVersion v ) { _lastmod = v; }

        //
        // split support
        //

        long getBytesWritten() const { return _dataWritten; }
        // Const since _dataWritten is mutable and a heuristic
        // TODO: Split data tracking and chunk information
        void setBytesWritten( long bytesWritten ) const { _dataWritten = bytesWritten; }

        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         * @return if something was split
         */
        bool splitIfShould( long dataWritten ) const;

        /**
         * Splits this chunk at a non-specificed split key to be chosen by the mongod holding this chunk.
         *
         * @param atMedian if set to true, will split the chunk at the middle regardless if
         *      the split is really necessary size wise. If set to false, will only split if
         *      the chunk has reached the currently desired maximum size. Setting to false also
         *      has the effect of splitting the chunk such that the resulting chunks will never
         *      be greater than the current chunk size setting.
         * @param res the object containing details about the split execution
         * @param resultingSplits the number of resulting split points. Set to NULL to ignore.
         *
         * @throws UserException
         */
        Status split( bool atMedian, size_t* resultingSplits ) const;

        /**
         * Splits this chunk at the given key (or keys)
         *
         * @param splitPoints the vector of keys that should be used to divide this chunk
         * @param res the object containing details about the split execution
         *
         * @throws UserException
         */
        Status multiSplit( const std::vector<BSONObj>& splitPoints ) const;

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
        void pickSplitVector( std::vector<BSONObj>& splitPoints , int chunkSize , int maxPoints = 0, int maxObjs = 0) const;

        //
        // migration support
        //

        /**
         * Issues a migrate request for this chunk
         *
         * @param to shard to move this chunk to
         * @param chunSize maximum number of bytes beyond which the migrate should no go trhough
         * @param secondaryThrottle whether during migrate all writes should block for repl
         * @param waitForDelete whether chunk move should wait for cleanup or return immediately
         * @param maxTimeMS max time for the migrate request
         * @param res the object containing details about the migrate execution
         * @return true if move was successful
         */
        bool moveAndCommit(const Shard& to,
                           long long chunkSize,
                           bool secondaryThrottle,
                           bool waitForDelete,
                           int maxTimeMS,
                           BSONObj& res) const;

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

        /**
         * sets MaxChunkSize
         * 1 <= newMaxChunkSize <= 1024
         * @return true if newMaxChunkSize is valid and was set
         */
        static bool setMaxChunkSizeSizeMB( int newMaxChunkSize );

        //
        // public constants
        //

        static int MaxChunkSize;
        static int MaxObjectPerChunk;
        static bool ShouldAutoSplit;

        //
        // accessors and helpers
        //

        std::string toString() const;

        friend std::ostream& operator << (std::ostream& out, const Chunk& c) { return (out << c.toString()); }

        // chunk equality is determined by comparing the min and max bounds of the chunk
        bool operator==(const Chunk& s) const;
        bool operator!=(const Chunk& s) const { return ! ( *this == s ); }

        std::string getns() const;
        Shard getShard() const { return _shard; }
        const ChunkManager* getManager() const { return _manager; }
        

    private:

        // main shard info
        
        const ChunkManager * _manager;

        BSONObj _min;
        BSONObj _max;
        Shard _shard;
        ChunkVersion _lastmod;
        mutable bool _jumbo;

        // transient stuff

        mutable long _dataWritten;

        // methods, etc..

        /** Returns the highest or lowest existing value in the shard-key space.
         *  Warning: this assumes that the shard key is not "special"- that is, the shardKeyPattern
         *           is simply an ordered list of ascending/descending field names. Examples:
         *           {a : 1, b : -1} is not special. {a : "hashed"} is.
         *
         * if sort 1, return lowest key
         * if sort -1, return highest key
         * will return empty object if have none
         */
        BSONObj _getExtremeKey( int sort ) const;

        /**
         * Determines the appropriate split points for this chunk.
         *
         * @param atMedian perform a single split at the middle of this chunk.
         * @param splitPoints out parameter containing the chosen split points. Can be empty.
         */
        void determineSplitPoints(bool atMedian, std::vector<BSONObj>* splitPoints) const;

        /** initializes _dataWritten with a random value so that a mongos restart wouldn't cause delay in splitting */
        static int mkDataWritten();

        ShardKeyPattern skey() const;
    };

    class ChunkRange {
    public:
        const ChunkManager* getManager() const { return _manager; }
        Shard getShard() const { return _shard; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }

        // clones of Chunk methods
        // Returns true if this ChunkRange contains the given point, and false otherwise
        //
        // Note: this function takes an extracted *key*, not an original document
        // (the point may be computed by, say, hashing a given field or projecting
        //  to a subset of fields).
        bool containsPoint( const BSONObj& point ) const;

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

        friend std::ostream& operator<<(std::ostream& out, const ChunkRange& cr) {
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
        typedef std::map<Shard,ChunkVersion> ShardVersionMap;

        // Loads a new chunk manager from a collection document
        ChunkManager( const BSONObj& collDoc );

        // Creates an empty chunk manager for the namespace
        ChunkManager( const std::string& ns, const ShardKeyPattern& pattern, bool unique );

        // Updates a chunk manager based on an older manager
        ChunkManager( ChunkManagerPtr oldManager );

        std::string getns() const { return _ns; }

        const ShardKeyPattern& getShardKey() const {  return _key; }

        bool hasShardKey( const BSONObj& obj ) const;

        bool isUnique() const { return _unique; }

        /**
         * this is just an increasing number of how many ChunkManagers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber() const { return _sequenceNumber; }

        //
        // After constructor is invoked, we need to call loadExistingRanges.  If this is a new
        // sharded collection, we can call createFirstChunks first.
        //

        // Creates new chunks based on info in chunk manager
        void createFirstChunks( const std::string& config,
                                const Shard& primary,
                                const std::vector<BSONObj>* initPoints,
                                const std::vector<Shard>* initShards );

        // Loads existing ranges based on info in chunk manager
        void loadExistingRanges( const std::string& config );


        // Helpers for load
        void calcInitSplitsAndShards( const Shard& primary,
                                      const std::vector<BSONObj>* initPoints,
                                      const std::vector<Shard>* initShards,
                                      std::vector<BSONObj>* splitPoints,
                                      std::vector<Shard>* shards ) const;

        //
        // Methods to use once loaded / created
        //

        int numChunks() const { return _chunkMap.size(); }

        /** Given a document, returns the chunk which contains that document.
         *  This works by extracting the shard key part of the given document, then
         *  calling findIntersectingChunk() on the extracted key.
         *
         *  See also the description for findIntersectingChunk().
         */
        ChunkPtr findChunkForDoc( const BSONObj& doc ) const;

        /** Given a key that has been extracted from a document, returns the
         *  chunk that contains that key.
         *
         *  For instance, to locate the chunk for document {a : "foo" , b : "bar"}
         *  when the shard key is {a : "hashed"}, you can call
         *      findChunkForDoc() on {a : "foo" , b : "bar"}, or
         *      findIntersectingChunk() on {a : hash("foo") }
         */
        ChunkPtr findIntersectingChunk( const BSONObj& point ) const;


        ChunkPtr findChunkOnServer( const Shard& shard ) const;

        void getShardsForQuery( std::set<Shard>& shards , const BSONObj& query ) const;
        void getAllShards( std::set<Shard>& all ) const;
        /** @param shards set to the shards covered by the interval [min, max], see SERVER-4791 */
        void getShardsForRange( std::set<Shard>& shards, const BSONObj& min, const BSONObj& max ) const;

        // Transforms query into bounds for each field in the shard key
        // for example :
        //   Key { a: 1, b: 1 },
        //   Query { a : { $gte : 1, $lt : 2 },
        //            b : { $gte : 3, $lt : 4 } }
        //   => Bounds { a : [1, 2), b : [3, 4) }
        static IndexBounds getIndexBoundsForQuery(const BSONObj& key, const CanonicalQuery* canonicalQuery);

        // Collapse query solution tree.
        //
        // If it has OR node, the result could be a superset of the index bounds generated.
        // Since to give a single IndexBounds, this gives the union of bounds on each field.
        // for example:
        //   OR: { a: (0, 1), b: (0, 1) },
        //       { a: (2, 3), b: (2, 3) }
        //   =>  { a: (0, 1), (2, 3), b: (0, 1), (2, 3) }
        static IndexBounds collapseQuerySolution( const QuerySolutionNode* node );

        ChunkMap getChunkMap() const { return _chunkMap; }

        /**
         * Returns true if, for this shard, the chunks are identical in both chunk managers
         */
        bool compatibleWith( const ChunkManager& other, const Shard& shard ) const;
        bool compatibleWith( ChunkManagerPtr other, const Shard& shard ) const { if( ! other ) return false; return compatibleWith( *other, shard ); }

        bool compatibleWith( const Chunk& other ) const;
        bool compatibleWith( ChunkPtr other ) const { if( ! other ) return false; return compatibleWith( *other ); }

        std::string toString() const;

        ChunkVersion getVersion( const StringData& shardName ) const;
        ChunkVersion getVersion( const Shard& shard ) const;
        ChunkVersion getVersion() const;

        void getInfo( BSONObjBuilder& b ) const;

        /**
         * @param me - so i don't get deleted before i'm done
         */
        void drop( ChunkManagerPtr me ) const;

        void _printChunks() const;

        int getCurrentDesiredChunkSize() const;

        ChunkManagerPtr reload(bool force=true) const; // doesn't modify self!

        void markMinorForReload( ChunkVersion majorVersion ) const;
        void getMarkedMinorVersions( std::set<ChunkVersion>& minorVersions ) const;

    private:

        // helpers for loading

        // returns true if load was consistent
        bool _load( const std::string& config, ChunkMap& chunks, std::set<Shard>& shards,
                                    ShardVersionMap& shardVersions, ChunkManagerPtr oldManager);
        static bool _isValid(const ChunkMap& chunks);

        // end helpers

        // All members should be const for thread-safety
        const std::string _ns;
        const ShardKeyPattern _key;
        const bool _unique;

        const ChunkMap _chunkMap;
        const ChunkRangeManager _chunkRanges;

        const std::set<Shard> _shards;

        const ShardVersionMap _shardVersions; // max version per shard

        // max version of any chunk
        ChunkVersion _version;

        // the previous manager this was based on
        // cleared after loading chunks
        ChunkManagerPtr _oldManager;

        mutable mutex _mutex; // only used with _nsLock

        const unsigned long long _sequenceNumber;

        //
        // Split Heuristic info
        //


        class SplitHeuristics {
        public:

            SplitHeuristics() :
                _splitTickets( maxParallelSplits ),
                _staleMinorSetMutex( "SplitHeuristics::staleMinorSet" ),
                _staleMinorCount( 0 ) {}

            void markMinorForReload( const std::string& ns, ChunkVersion majorVersion );
            void getMarkedMinorVersions( std::set<ChunkVersion>& minorVersions );

            TicketHolder _splitTickets;

            mutex _staleMinorSetMutex;

            // mutex protects below
            int _staleMinorCount;
            std::set<ChunkVersion> _staleMinorSet;

            // Test whether we should split once data * splitTestFactor > chunkSize (approximately)
            static const int splitTestFactor = 5;
            // Maximum number of parallel threads requesting a split
            static const int maxParallelSplits = 5;

            // The idea here is that we're over-aggressive on split testing by a factor of
            // splitTestFactor, so we can safely wait until we get to splitTestFactor invalid splits
            // before changing.  Unfortunately, we also potentially over-request the splits by a
            // factor of maxParallelSplits, but since the factors are identical it works out
            // (for now) for parallel or sequential oversplitting.
            // TODO: Make splitting a separate thread with notifications?
            static const int staleMinorReloadThreshold = maxParallelSplits;

        };

        mutable SplitHeuristics _splitHeuristics;

        //
        // End split heuristics
        //

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
    inline std::string Chunk::genID() const { return genID(_manager->getns(), _min); }

    bool setShardVersion( DBClientBase & conn,
                          const std::string& ns,
                          ChunkVersion version,
                          ChunkManagerPtr manager,
                          bool authoritative,
                          BSONObj& result );

} // namespace mongo
