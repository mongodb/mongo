// balancer_policy.cpp

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

#include "pch.h"

#include "config.h"

#include "../util/stringutils.h"

#include "balancer_policy.h"

namespace mongo {

    // limits map fields
    BSONField<long long> LimitsFields::currSize( "currSize" );
    BSONField<bool> LimitsFields::hasOpsQueued( "hasOpsQueued" );

    BalancerPolicy::ChunkInfo* BalancerPolicy::balance( const string& ns,
            const ShardToLimitsMap& shardToLimitsMap,
            const ShardToChunksMap& shardToChunksMap,
            int balancedLastTime ) {
        pair<string,unsigned> min("",numeric_limits<unsigned>::max());
        pair<string,unsigned> max("",0);
        vector<string> drainingShards;

        bool maxOpsQueued = false;

        for (ShardToChunksIter i = shardToChunksMap.begin(); i!=shardToChunksMap.end(); ++i ) {

            // Find whether this shard's capacity or availability are exhausted
            const string& shard = i->first;
            BSONObj shardLimits;
            ShardToLimitsIter it = shardToLimitsMap.find( shard );
            if ( it != shardToLimitsMap.end() ) shardLimits = it->second;
            const bool maxedOut = isSizeMaxed( shardLimits );
            const bool draining = isDraining( shardLimits );
            const bool opsQueued = hasOpsQueued( shardLimits );

            
            // Is this shard a better chunk receiver then the current one?
            // Shards that would be bad receiver candidates:
            // + maxed out shards
            // + draining shards
            // + shards with operations queued for writeback
            const unsigned size = i->second.size();
            if ( ! maxedOut && ! draining && ! opsQueued ) {
                if ( size < min.second ) {
                    min = make_pair( shard , size );
                }
            }
            else if ( opsQueued ) {
                LOG(1) << "won't send a chunk to: " << shard << " because it has ops queued" << endl;
            }
            else if ( maxedOut ) {
                LOG(1) << "won't send a chunk to: " << shard << " because it is maxedOut" << endl;
            }


            // Check whether this shard is a better chunk donor then the current one.
            // Draining shards take a lower priority than overloaded shards.
            if ( size > max.second ) {
                max = make_pair( shard , size );
                maxOpsQueued = opsQueued;
            }
            if ( draining && (size > 0)) {
                drainingShards.push_back( shard );
            }
        }

        // If there is no candidate chunk receiver -- they may have all been maxed out,
        // draining, ... -- there's not much that the policy can do.
        if ( min.second == numeric_limits<unsigned>::max() ) {
            log() << "no available shards to take chunks" << endl;
            return NULL;
        }

        if ( maxOpsQueued ) {
            log() << "biggest shard " << max.first << " has unprocessed writebacks, waiting for completion of migrate" << endl;
            return NULL;
        }

        LOG(1) << "collection : " << ns << endl;
        LOG(1) << "donor      : " << max.second << " chunks on " << max.first << endl;
        LOG(1) << "receiver   : " << min.second << " chunks on " << min.first << endl;
        if ( ! drainingShards.empty() ) {
            string drainingStr;
            joinStringDelim( drainingShards, &drainingStr, ',' );
            LOG(1) << "draining           : " << ! drainingShards.empty() << "(" << drainingShards.size() << ")" << endl;
        }

        // Solving imbalances takes a higher priority than draining shards. Many shards can
        // be draining at once but we choose only one of them to cater to per round.
        // Important to start balanced, so when there are few chunks any imbalance must be fixed.
        const int imbalance = max.second - min.second;
        int threshold = 8;
        if (balancedLastTime || max.second < 20) threshold = 2;
        else if (max.second < 80) threshold = 4;
        string from, to;
        if ( imbalance >= threshold ) {
            from = max.first;
            to = min.first;

        }
        else if ( ! drainingShards.empty() ) {
            from = drainingShards[ rand() % drainingShards.size() ];
            to = min.first;

        }
        else {
            // Everything is balanced here!
            return NULL;
        }

        const vector<BSONObj>& chunksFrom = shardToChunksMap.find( from )->second;
        const vector<BSONObj>& chunksTo = shardToChunksMap.find( to )->second;
        BSONObj chunkToMove = pickChunk( chunksFrom , chunksTo );
        log() << "chose [" << from << "] to [" << to << "] " << chunkToMove << endl;

        return new ChunkInfo( ns, to, from, chunkToMove );
    }

    BSONObj BalancerPolicy::pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to ) {
        // It is possible for a donor ('from') shard to have less chunks than a receiver one ('to')
        // if the donor is in draining mode.

        if ( to.size() == 0 )
            return from[0];

        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }

    bool BalancerPolicy::isSizeMaxed( BSONObj limits ) {
        // If there's no limit information for the shard, assume it can be a chunk receiver
        // (i.e., there's not bound on space utilization)
        if ( limits.isEmpty() ) {
            return false;
        }

        long long maxUsage = limits[ ShardFields::maxSize.name() ].Long();
        if ( maxUsage == 0 ) {
            return false;
        }

        long long currUsage = limits[ LimitsFields::currSize.name() ].Long();
        if ( currUsage < maxUsage ) {
            return false;
        }

        return true;
    }

    bool BalancerPolicy::isDraining( BSONObj limits ) {
        BSONElement draining = limits[ ShardFields::draining.name() ];
        if ( draining.eoo() || ! draining.trueValue() ) {
            return false;
        }

        return true;
    }

    bool BalancerPolicy::hasOpsQueued( BSONObj limits ) {
        BSONElement opsQueued = limits[ LimitsFields::hasOpsQueued.name() ];
        if ( opsQueued.eoo() || ! opsQueued.trueValue() ) {
            return false;
        }
        return true;
    }

}  // namespace mongo
