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

#include "../client/dbclient.h"
#include "../util/unittest.h"

#include "balancer_policy.h"

namespace mongo {

    BalancerPolicy::ChunkInfo* BalancerPolicy::balance( const string& ns, 
                                                        const ShardToLimitsMap& shardToLimitsMap,  
                                                        const ShardToChunksMap& shardToChunksMap, 
                                                        int balancedLastTime ){
        pair<string,unsigned> min("",numeric_limits<unsigned>::max());
        pair<string,unsigned> max("",0);
        vector<string> drainingShards;
	        
        for (ShardToChunksIter i = shardToChunksMap.begin(); i!=shardToChunksMap.end(); ++i ){

            // Find whether this shard has reached its size cap or whether it is being removed.
            const string& shard = i->first;
            BSONObj shardLimits;
            ShardToLimitsIter it = shardToLimitsMap.find( shard );
            if ( it != shardToLimitsMap.end() ) shardLimits = it->second;
            const bool maxedOut = isSizeMaxed( shardLimits );
            const bool draining = isDraining( shardLimits );

            // Check whether this shard is a better chunk receiver then the current one. 
            // Maxed out shards or draining shards cannot be considered receivers.
            const unsigned size = i->second.size();
            if ( ! maxedOut && ! draining ){
                if ( size < min.second ){
                    min = make_pair( shard , size );
                }
            }

            // Check whether this shard is a better chunk donor then the current one.
            // Draining shards take a lower priority than overloaded shards.
            if ( size > max.second ){
                max = make_pair( shard , size ); 
            }
            if ( draining && (size > 0)){
                drainingShards.push_back( shard );
            }
        }

        // If there is no candidate chunk receiver -- they may have all been maxed out, 
        // draining, ... -- there's not much that the policy can do.  
        if ( min.second == numeric_limits<unsigned>::max() ){
            log() << "balancer: no availalable shards to take chunks" << endl;
            return NULL;
        }
        
        log(4) << "min: " << min.first << "\t" << min.second << endl;
        log(4) << "max: " << max.first << "\t" << max.second << endl;
        log(4) << "draining: " << ! drainingShards.empty() << "(" << drainingShards.size() << ")" << endl;

        // Solving imbalances takes a higher priority than draining shards. Many shards can
        // be draining at once but we choose only one of them to cater to per round.
        const int imbalance = max.second - min.second;
        const int threshold = balancedLastTime ? 2 : 8;
        string from, to;
        if ( imbalance >= threshold ){
            from = max.first;
            to = min.first;

        } else if ( ! drainingShards.empty() ){
            from = drainingShards[ rand() % drainingShards.size() ];
            to = min.first;

        } else {
            // Everything is balanced here! 
            return NULL;
        }

        const vector<BSONObj>& chunksFrom = shardToChunksMap.find( from )->second;
        const vector<BSONObj>& chunksTo = shardToChunksMap.find( to )->second;
        BSONObj chunkToMove = pickChunk( chunksFrom , chunksTo );
        log() << "balancer: chose [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        return new ChunkInfo( ns, to, from, chunkToMove );
    }

    BSONObj BalancerPolicy::pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to ){
        // It is possible for a donor ('from') shard to have less chunks than a recevier one ('to')
        // if the donor is in draining mode. 
        
        if ( to.size() == 0 )
            return from[0];
        
        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }

    bool BalancerPolicy::isSizeMaxed( BSONObj limits ){
        // If there's no limit information for the shard, assume it can be a chunk receiver 
        // (i.e., there's not bound on space utilization)
        if ( limits.isEmpty() ){
            return false;
        }

        long long maxUsage = limits[ ShardFields::maxSize.name() ].Long();
        if ( maxUsage == 0 ){
            return false;
        }

        long long currUsage = limits[ ShardFields::currSize.name() ].Long();
        if ( currUsage < maxUsage ){
            return false;
        }

        return true;
    }

    bool BalancerPolicy::isDraining( BSONObj limits ){
        BSONElement draining = limits[ ShardFields::draining.name() ];
        if ( draining.eoo() || ! draining.Bool() ){
            return false;
        }

        return true;
    }

    class PolicyObjUnitTest : public UnitTest {
    public:

        typedef ShardFields sf;  // convenience alias

        void caseSizeMaxedShard(){
            BSONObj shard0 = BSON( sf::maxSize(0LL) << sf::currSize(0LL) );
            assert( ! BalancerPolicy::isSizeMaxed( shard0 ) );

            BSONObj shard1 = BSON( sf::maxSize(100LL) << sf::currSize(80LL) );
            assert( ! BalancerPolicy::isSizeMaxed( shard1 ) );

            BSONObj shard2 = BSON( sf::maxSize(100LL) << sf::currSize(110LL) );
            assert( BalancerPolicy::isSizeMaxed( shard2 ) );

            BSONObj empty;
            assert( ! BalancerPolicy::isSizeMaxed( empty ) );
        }

        void caseDrainingShard(){
            BSONObj shard0 = BSON( sf::draining(true) );
            assert( BalancerPolicy::isDraining( shard0 ) );

            BSONObj shard1 = BSON( sf::draining(false) );
            assert( ! BalancerPolicy::isDraining( shard1 ) );

            BSONObj empty;
            assert( ! BalancerPolicy::isDraining( empty ) );
        }

        void caseBalanceNormal(){
            // 2 chunks and 0 chunk shards
            BalancerPolicy::ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunkMap["shard1"] = chunks;

            // no limits
            BalancerPolicy::ShardToLimitsMap limitsMap;
            BSONObj limits0 = BSON( sf::maxSize(0LL) << sf::currSize(2LL) << sf::draining(false) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << sf::currSize(0LL) << sf::draining(false) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;
	    
            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 1 );
            assert( c != NULL );
        }

        void caseBalanceDraining(){
            // one normal, one draining
            // 2 chunks and 0 chunk shards
            BalancerPolicy::ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
            chunkMap["shard1"] = chunks;

            // shard0 is draining
            BalancerPolicy::ShardToLimitsMap limitsMap;
            BSONObj limits0 = BSON( sf::maxSize(0LL) << sf::currSize(2LL) << sf::draining(true) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << sf::currSize(0LL) << sf::draining(false) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;
	    
            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            assert( c != NULL );
            assert( c->to == "shard1" );
            assert( c->from == "shard0" );
            assert( ! c->chunk.isEmpty() );
        }

        void caseBalanceEndedDraining(){
            // 2 chunks and 0 chunk (drain completed) shards
            BalancerPolicy::ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunkMap["shard1"] = chunks;

            // no limits
            BalancerPolicy::ShardToLimitsMap limitsMap;
            BSONObj limits0 = BSON( sf::maxSize(0LL) << sf::currSize(2LL) << sf::draining(false) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << sf::currSize(0LL) << sf::draining(true) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;
	    
            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            assert( c == NULL );	    
        }

        void caseBalanceImpasse(){
            // one maxed out, one draining
            // 2 chunks and 0 chunk shards
            BalancerPolicy::ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
            chunkMap["shard1"] = chunks;

            // shard0 is draining, shard1 is maxed out
            BalancerPolicy::ShardToLimitsMap limitsMap;
            BSONObj limits0 = BSON( sf::maxSize(0LL) << sf::currSize(2LL) << sf::draining(true) );
            BSONObj limits1 = BSON( sf::maxSize(1LL) << sf::currSize(1LL) << sf::draining(false) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;
	    
            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            assert( c == NULL );
        }

        void run(){
            caseSizeMaxedShard();
            caseDrainingShard();
            caseBalanceNormal();
            caseBalanceDraining();
            caseBalanceImpasse();
            log(1) << "policyObjUnitTest passed" << endl;
        }
    } policyObjUnitTest;

}  // namespace mongo
