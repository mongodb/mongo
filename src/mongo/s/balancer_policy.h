// @file balancer_policy.h

/**
*    Copyright (C) 2010 10gen Inc.
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

#ifndef S_BALANCER_POLICY_HEADER
#define S_BALANCER_POLICY_HEADER

#include "mongo/db/jsobj.h"
#include "mongo/s/type_chunk.h"

namespace mongo {


    struct ChunkInfo {
        const BSONObj min;
        const BSONObj max;
        
        ChunkInfo( const BSONObj& a_min, const BSONObj& a_max ) 
            : min( a_min.getOwned() ), max( a_max.getOwned() ){}

        ChunkInfo( const BSONObj& chunk )
            : min(chunk[ChunkType::min()].Obj().getOwned()),
              max(chunk[ChunkType::max()].Obj().getOwned()) {
        }

        string toString() const;
    };

    struct TagRange {
        BSONObj min;
        BSONObj max;
        string tag;
        
        TagRange(){}

        TagRange( const BSONObj& a_min, const BSONObj& a_max, const string& a_tag ) 
            : min( a_min.getOwned() ), max( a_max.getOwned() ), tag( a_tag ){}
        string toString() const;
    };
    
    class ShardInfo {
    public:
        ShardInfo();
        ShardInfo( long long maxSize, long long currSize, 
                   bool draining, bool opsQueued, 
                   const set<string>& tags = set<string>(),
                   const string& _mongoVersion = string("") );

        void addTag( const string& tag );

        /** @return true if we have the tag OR if the tag is "" */
        bool hasTag( const string& tag ) const;
        
        /**
         * @return true if a shard cannot receive any new chunks because it reaches 'shardLimits'.
         * Expects the optional fields "maxSize", can in size in MB, and "usedSize", currently used size
         * in MB, on 'shardLimits'.
         */
        bool isSizeMaxed() const;
        
        /**
         * @return true if 'shardLimist' contains a field "draining". Expects the optional field
         * "isDraining" on 'shrdLimits'.
         */
        bool isDraining() const { return _draining; }
        
        /**
         * @return true if a shard currently has operations in any of its writeback queues
         */
        bool hasOpsQueued() const { return _hasOpsQueued; }
        
        long long getMaxSize() const { return _maxSize; }

        long long getCurrSize() const { return _currSize; }

        string getMongoVersion() const { return _mongoVersion; }

        string toString() const;
        
    private:
        long long _maxSize;
        long long _currSize;
        bool _draining;
        bool _hasOpsQueued;
        set<string> _tags;
        string _mongoVersion;
    };
    
    struct MigrateInfo {
        const string ns;
        const string to;
        const string from;
        const ChunkInfo chunk;

        MigrateInfo( const string& a_ns , const string& a_to , const string& a_from , const BSONObj& a_chunk )
            : ns( a_ns ) , to( a_to ) , from( a_from ), chunk( a_chunk ) {}


    };

    typedef map< string,ShardInfo > ShardInfoMap;
    typedef map< string,vector<BSONObj> > ShardToChunksMap;

    class DistributionStatus : boost::noncopyable {
    public:
        DistributionStatus( const ShardInfoMap& shardInfo,
                            const ShardToChunksMap& shardToChunksMap );

        // only used when building
        
        /**
         * @return if range is valid
         */
        bool addTagRange( const TagRange& range );

        // ---- these methods might be better suiting in BalancerPolicy
        
        /**
         * @param forTag "" if you don't care, or a tag
         * @return shard best suited to receive a chunk
         */
        string getBestReceieverShard( const string& forTag ) const;

        /**
         * @return the shard with the most chunks
         *         based on # of chunks with the given tag
         */
        string getMostOverloadedShard( const string& forTag ) const;


        // ---- basic accessors, counters, etc...

        /** @return total number of chunks  */
        unsigned totalChunks() const;

        /** @return number of chunks in this shard */
        unsigned numberOfChunksInShard( const string& shard ) const;

        /** @return number of chunks in this shard with the given tag */
        unsigned numberOfChunksInShardWithTag( const string& shard, const string& tag ) const;

        /** @return chunks for the shard */
        const vector<BSONObj>& getChunks( const string& shard ) const;

        /** @return all tags we know about, not include "" */
        const set<string>& tags() const { return _allTags; }

        /** @return the right tag for chunk, possibly "" */
        string getTagForChunk( const BSONObj& chunk ) const;
        
        /** @return all shards we know about */
        const set<string>& shards() const { return _shards; }

        /** @return the ShardInfo for the shard */
        const ShardInfo& shardInfo( const string& shard ) const;
        
        /** writes all state to log() */
        void dump() const;
        
    private:
        const ShardInfoMap& _shardInfo;
        const ShardToChunksMap& _shardChunks;
        map<BSONObj,TagRange> _tagRanges;
        set<string> _allTags;
        set<string> _shards;
    };

    class BalancerPolicy {
    public:

        /**
         * Returns a suggested chunk to move whithin a collection's shards, given information about
         * space usage and number of chunks for that collection. If the policy doesn't recommend
         * moving, it returns NULL.
         *
         * @param ns is the collections namepace.
         * @param DistributionStatus holds all the info about the current state of the cluster/namespace
         * @param balancedLastTime is the number of chunks effectively moved in the last round.
         * @returns NULL or MigrateInfo of the best move to make towards balacing the collection.
         *          caller owns the MigrateInfo instance
         */
        static MigrateInfo* balance( const string& ns,
                                     const DistributionStatus& distribution,
                                     int balancedLastTime );

    private:
        static bool _isJumbo( const BSONObj& chunk );
    };



}  // namespace mongo

#endif  // S_BALANCER_POLICY_HEADER
