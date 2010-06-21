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
                                                        const map< string, BSONObj>& shardLimitsMap,  
                                                        const map< string,vector<BSONObj> >& shardToChunksMap, 
                                                        int balancedLastTime ){
        pair<string,unsigned> min("",9999999);
        pair<string,unsigned> max("",0);
        
        for ( map< string,vector<BSONObj> >::const_iterator i=shardToChunksMap.begin(); i!=shardToChunksMap.end(); ++i ){
            const string& shard = i->first;
            unsigned size = i->second.size();

            BSONObj shardLimits;
            map< string,BSONObj >::const_iterator it = shardLimitsMap.find( shard );
            if ( it != shardLimitsMap.end() ) {
                shardLimits = it->second;
            }

            if ( size < min.second ){
                if ( isReceiver( shardLimits ) ){
                    min.first = shard;
                    min.second = size;
                } else {
                    log() << "balancer: shard can't receive any more chunks (" << shard  << ")" << endl;
                }
            }
            
            if ( isDraining( shardLimits ) || ( size > max.second )){
                max.first = shard;
                max.second = size;
            }
        }
        
        log(4) << "min: " << min.first << "\t" << min.second << endl;
        log(4) << "max: " << max.first << "\t" << max.second << endl;
        
        if( (int)( max.second - min.second) < ( balancedLastTime ? 1 : 1 ) )
            return NULL;

        const string from = max.first;
        const string to = min.first;
        const vector<BSONObj>& chunksFrom = shardToChunksMap.find( from )->second;
        const vector<BSONObj>& chunksTo = shardToChunksMap.find( to )->second;  
        BSONObj chunkToMove = pickChunk( chunksFrom , chunksTo );

        log() << "balancer: chose chunk from [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        return new ChunkInfo( ns, to, from, chunkToMove );
    }

    BSONObj BalancerPolicy::pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to ){
        assert( from.size() > to.size() );
        
        if ( to.size() == 0 )
            return from[0];
        
        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }

    bool BalancerPolicy::isReceiver( BSONObj limits ){

        // A draining shard can never be a receiver
        if ( isDraining( limits ) ){
            return false;
        }

        // If there's no limit information for the shard, assume it can be a chunk receiver 
        // (i.e., there's not bound on space utilization)
        if ( limits.isEmpty() ){
            return true;
        }

        long long maxUsage = limits["maxSize"].Long();
        if ( maxUsage == 0 ){
            return true;
        }

        long long currUsage = limits["currSize"].Long();
        if ( currUsage < maxUsage ){
            return true;
        }

        return false;
    }

    bool BalancerPolicy::isDraining( BSONObj limits ){

        // If there's no entry saying it is draining, it isn't.
        if ( limits.isEmpty() || limits[ "draining" ].eoo() ){
            return false;
        }

        return true;
    }

    class PolicyObjUnitTest : public UnitTest {
    public:
        void maxSizeForShard(){
            BSONObj shard0 = BSON( "maxSize" << 0LL << "currSize" << 0LL );
            assert( BalancerPolicy::isReceiver( shard0 ) );

            BSONObj shard1 = BSON( "maxSize" << 100LL << "currSize" << 80LL );
            assert( BalancerPolicy::isReceiver( shard1 ) );

            BSONObj shard2 = BSON( "maxSize" << 100LL << "currSize" << 110LL );
            assert( ! BalancerPolicy::isReceiver( shard2 ) );

            BSONObj shard3 = BSON( "draining" << true );
            assert( ! BalancerPolicy::isReceiver( shard3 ) );

            BSONObj empty;
            assert( BalancerPolicy::isReceiver( empty ) );
        }

        void drainingShard(){
            BSONObj shard0 = BSON( "draining" << true );
            assert( BalancerPolicy::isDraining( shard0 ) );

            BSONObj empty;
            assert( ! BalancerPolicy::isDraining( empty ) );
        }

        void run(){
            maxSizeForShard();
            log(1) << "policyObjUnitTest passed" << endl;
        }
    } policyObjUnitTest;

}  // namespace mongo
