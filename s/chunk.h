// shard.h

/*
   A "shard" is a database (replica pair typically) which represents
   one partition of the overall database.
*/

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
#include "../client/dbclient.h"
#include "../client/model.h"
#include "../bson/util/atomic_int.h"
#include "shardkey.h"
#include "shard.h"
#include "config.h"
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
        
        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        
        void setMin(const BSONObj& o){
            _min = o;
        }
        void setMax(const BSONObj& o){
            _max = o;
        }


        string getns() const;
        Shard getShard() const { return _shard; }

        void setShard( const Shard& shard );

        bool contains( const BSONObj& obj ) const;

        string toString() const;

        friend ostream& operator << (ostream& out, const Chunk& c){ return (out << c.toString()); }

        bool operator==(const Chunk& s) const;
        
        bool operator!=(const Chunk& s) const{
            return ! ( *this == s );
        }
        
        // if min/max key is pos/neg infinity
        bool minIsInf() const;
        bool maxIsInf() const;

        BSONObj pickSplitPoint() const;
        ChunkPtr split();

        void pickSplitVector( vector<BSONObj>* splitPoints , int chunkSize ) const;
        ChunkPtr multiSplit( const vector<BSONObj>& splitPoints );

        /**
         * @return size of shard in bytes
         *  talks to mongod to do this
         */
        long getPhysicalSize() const;
        
        int countObjects(int maxcount=0) const;
        
        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         */
        bool splitIfShould( long dataWritten );
        
        /*
         * moves either this shard or newShard if it makes sense too
         * @return whether or not a shard was moved
         */
        bool moveIfShould( ChunkPtr newShard = ChunkPtr() );

        bool moveAndCommit( const Shard& to , string& errmsg );

        const char * getNS(){ return "config.chunks"; }
        void serialize(BSONObjBuilder& to, ShardChunkVersion myLastMod=0);
        void unserialize(const BSONObj& from);
        string modelServer() const;
        
        void appendShortVersion( const char * name , BSONObjBuilder& b );

        void _markModified();
        
        static int MaxChunkSize;

        string genID() const;
        static string genID( const string& ns , const BSONObj& min );

        const ChunkManager* getManager() const { return _manager; }
        
        bool modified();

        ShardChunkVersion getVersionOnConfigServer() const;
    private:

        bool _splitIfShould( long dataWritten );

        // main shard info
        
        ChunkManager * _manager;
        ShardKeyPattern skey() const;

        BSONObj _min;
        BSONObj _max;
        Shard _shard;
        ShardChunkVersion _lastmod;

        bool _modified;
        
        // transient stuff

        long _dataWritten;
        
        // methods, etc..
        
        void _split( BSONObj& middle );

        friend class ChunkManager;
        friend class ShardObjUnitTest;
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

        ChunkManager( DBConfig * config , string ns , ShardKeyPattern pattern , bool unique );
        virtual ~ChunkManager();

        string getns() const { return _ns; }
        
        int numChunks() const { rwlock lk( _lock , false ); return _chunkMap.size(); }
        bool hasShardKey( const BSONObj& obj );

        ChunkPtr findChunk( const BSONObj& obj , bool retry = false );
        ChunkPtr findChunkOnServer( const Shard& shard ) const;
        
        ShardKeyPattern& getShardKey(){  return _key; }
        const ShardKeyPattern& getShardKey() const {  return _key; }
        bool isUnique(){ return _unique; }

        void maybeChunkCollection();
        
        void getShardsForQuery( set<Shard>& shards , const BSONObj& query );
        void getAllShards( set<Shard>& all );
        void getShardsForRange(set<Shard>& shards, const BSONObj& min, const BSONObj& max); // [min, max)

        void save( bool major );

        string toString() const;

        ShardChunkVersion getVersion( const Shard& shard ) const;
        ShardChunkVersion getVersion() const;

        /** 
         * actually does a query on the server
         * doesn't look at any local data
         */
        ShardChunkVersion getVersionOnConfigServer() const;
        
        /**
         * this is just an increasing number of how many ChunkManagers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber(){
            return _sequenceNumber;
        }
        
        void getInfo( BSONObjBuilder& b ){
            b.append( "key" , _key.key() );
            b.appendBool( "unique" , _unique );
        }
        
        /**
         * @param me - so i don't get deleted before i'm done
         */
        void drop( ChunkManagerPtr me );

        void _printChunks() const;
        
    private:
        
        void _reload();
        void _reload_inlock();
        void _load();

        void save_inlock( bool major );
        ShardChunkVersion getVersion_inlock() const;
        void ensureIndex_inlock();
        
        DBConfig * _config;
        string _ns;
        ShardKeyPattern _key;
        bool _unique;
        
        map<string,unsigned long long> _maxMarkers;

        ChunkMap _chunkMap;
        ChunkRangeManager _chunkRanges;

        set<Shard> _shards;

        unsigned long long _sequenceNumber;
        
        mutable RWLock _lock;

        // This should only be called from Chunk after it has been migrated
        void _migrationNotification(Chunk* c);

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

} // namespace mongo
