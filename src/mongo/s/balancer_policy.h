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

#include "mongo/base/owned_pointer_vector.h"
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

        std::string toString() const;
    };

    struct TagRange {
        BSONObj min;
        BSONObj max;
        std::string tag;
        
        TagRange(){}

        TagRange( const BSONObj& a_min, const BSONObj& a_max, const std::string& a_tag )
            : min( a_min.getOwned() ), max( a_max.getOwned() ), tag( a_tag ){}
        std::string toString() const;
    };
    
    class ShardInfo {
    public:
        ShardInfo();
        ShardInfo( long long maxSize, long long currSize, 
                   bool draining, bool opsQueued, 
                   const std::set<std::string>& tags = std::set<std::string>(),
                   const std::string& _mongoVersion = std::string("") );

        void addTag( const std::string& tag );

        /** @return true if we have the tag OR if the tag is "" */
        bool hasTag( const std::string& tag ) const;
        
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

        std::string getMongoVersion() const { return _mongoVersion; }

        std::string toString() const;
        
    private:
        long long _maxSize;
        long long _currSize;
        bool _draining;
        bool _hasOpsQueued;
        std::set<std::string> _tags;
        std::string _mongoVersion;
    };
    
    struct MigrateInfo {
        const std::string ns;
        const std::string to;
        const std::string from;
        const ChunkInfo chunk;

        MigrateInfo( const std::string& a_ns , const std::string& a_to , const std::string& a_from , const BSONObj& a_chunk )
            : ns( a_ns ) , to( a_to ) , from( a_from ), chunk( a_chunk ) {}


    };

    typedef std::map< std::string,ShardInfo > ShardInfoMap;
    typedef std::map<std::string, OwnedPointerVector<ChunkType>* > ShardToChunksMap;

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
        std::string getBestReceieverShard( const std::string& forTag ) const;

        /**
         * @return the shard with the most chunks
         *         based on # of chunks with the given tag
         */
        std::string getMostOverloadedShard( const std::string& forTag ) const;


        // ---- basic accessors, counters, etc...

        /** @return total number of chunks  */
        unsigned totalChunks() const;

        /** @return number of chunks in this shard */
        unsigned numberOfChunksInShard( const std::string& shard ) const;

        /** @return number of chunks in this shard with the given tag */
        unsigned numberOfChunksInShardWithTag( const std::string& shard, const std::string& tag ) const;

        /** @return chunks for the shard */
        const std::vector<ChunkType*>& getChunks(const std::string& shard) const;

        /** @return all tags we know about, not include "" */
        const std::set<std::string>& tags() const { return _allTags; }

        /** @return the right tag for chunk, possibly "" */
        std::string getTagForChunk(const ChunkType& chunk) const;
        
        /** @return all shards we know about */
        const std::set<std::string>& shards() const { return _shards; }

        /** @return the ShardInfo for the shard */
        const ShardInfo& shardInfo( const std::string& shard ) const;
        
        /** writes all state to log() */
        void dump() const;
        
    private:
        const ShardInfoMap& _shardInfo;
        const ShardToChunksMap& _shardChunks;
        std::map<BSONObj,TagRange> _tagRanges;
        std::set<std::string> _allTags;
        std::set<std::string> _shards;
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
        static MigrateInfo* balance( const std::string& ns,
                                     const DistributionStatus& distribution,
                                     int balancedLastTime );
    };



}  // namespace mongo

#endif  // S_BALANCER_POLICY_HEADER
