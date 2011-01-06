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
*/

#ifndef S_BALANCER_POLICY_HEADER
#define S_BALANCER_POLICY_HEADER

#include "../pch.h"

namespace mongo {

    class BalancerPolicy {
    public:
        struct ChunkInfo;

        /**
         * Returns a suggested chunk to move whithin a collection's shards, given information about
         * space usage and number of chunks for that collection. If the policy doesn't recommend
         * moving, it returns NULL.
         *
         * @param ns is the collections namepace.
         * @param shardLimitMap is a map from shardId to an object that describes (for now) space
         * cap and usage. E.g.: { "maxSize" : <size_in_MB> , "usedSize" : <size_in_MB> }.
         * @param shardToChunksMap is a map from shardId to chunks that live there. A chunk's format
         * is { }.
         * @param balancedLastTime is the number of chunks effectively moved in the last round.
         * @returns NULL or ChunkInfo of the best move to make towards balacing the collection.
         */
        typedef map< string,BSONObj > ShardToLimitsMap;
        typedef map< string,vector<BSONObj> > ShardToChunksMap;
        static ChunkInfo* balance( const string& ns, const ShardToLimitsMap& shardToLimitsMap,
                                   const ShardToChunksMap& shardToChunksMap, int balancedLastTime );

        // below exposed for testing purposes only -- treat it as private --

        static BSONObj pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to );

        /**
         * Returns true if a shard cannot receive any new chunks bacause it reache 'shardLimits'.
         * Expects the optional fields "maxSize", can in size in MB, and "usedSize", currently used size
         * in MB, on 'shardLimits'.
         */
        static bool isSizeMaxed( BSONObj shardLimits );

        /**
         * Returns true if 'shardLimist' contains a field "draining". Expects the optional field
         * "isDraining" on 'shrdLimits'.
         */
        static bool isDraining( BSONObj shardLimits );

        /**
         * Returns true if a shard currently has operations in any of its writeback queues
         */
        static bool hasOpsQueued( BSONObj shardLimits );

    private:
        // Convenience types
        typedef ShardToChunksMap::const_iterator ShardToChunksIter;
        typedef ShardToLimitsMap::const_iterator ShardToLimitsIter;

    };

    struct BalancerPolicy::ChunkInfo {
        const string ns;
        const string to;
        const string from;
        const BSONObj chunk;

        ChunkInfo( const string& a_ns , const string& a_to , const string& a_from , const BSONObj& a_chunk )
            : ns( a_ns ) , to( a_to ) , from( a_from ), chunk( a_chunk ) {}
    };

    /**
     * Field names used in the 'limits' map.
     */
    struct LimitsFields {
        // we use 'draining' and 'maxSize' from the 'shards' collection plus the following
        static BSONField<long long> currSize; // currently used disk space in bytes
        static BSONField<bool> hasOpsQueued;  // writeback queue is not empty?
    };

}  // namespace mongo

#endif  // S_BALANCER_POLICY_HEADER
