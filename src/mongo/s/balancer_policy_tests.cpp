/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/unittest/unittest.h"
#include "mongo/s/config.h"
#include "mongo/s/balancer_policy.h"

namespace mongo {

    CmdLine cmdLine;
    bool inShutdown() { return false; }
    void setupSignals( bool inFork ) {}
    DBClientBase *createDirectClient() { return 0; }
    void dbexit( ExitCode rc, const char *why ){
        log()  << "dbexit called? :(" << endl;
        ::_exit(-1);
    }


    namespace {
        

        typedef mongo::ShardFields sf;  // fields from 'shards' colleciton
        typedef mongo::LimitsFields lf; // fields from the balancer's limits map

        TEST( BalancerPolicyTests , SizeMaxedShardTest ) {
            BSONObj shard0 = BSON( sf::maxSize(0LL) << lf::currSize(0LL) );
            ASSERT( ! BalancerPolicy::isSizeMaxed( shard0 ) );

            BSONObj shard1 = BSON( sf::maxSize(100LL) << lf::currSize(80LL) );
            ASSERT( ! BalancerPolicy::isSizeMaxed( shard1 ) );

            BSONObj shard2 = BSON( sf::maxSize(100LL) << lf::currSize(110LL) );
            ASSERT( BalancerPolicy::isSizeMaxed( shard2 ) );

            BSONObj empty;
            ASSERT( ! BalancerPolicy::isSizeMaxed( empty ) );
        }

        TEST( BalancerPolicyTests , DrainingShardTest ) {
            BSONObj shard0 = BSON( sf::draining(true) );
            ASSERT( BalancerPolicy::isDraining( shard0 ) );

            BSONObj shard1 = BSON( sf::draining(false) );
            ASSERT( ! BalancerPolicy::isDraining( shard1 ) );

            BSONObj empty;
            ASSERT( ! BalancerPolicy::isDraining( empty ) );
        }


        TEST( BalancerPolicyTests , BalanceNormalTest  ) {
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
            BSONObj limits0 = BSON( sf::maxSize(0LL) << lf::currSize(2LL) << sf::draining(false) << lf::hasOpsQueued(false) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << lf::currSize(0LL) << sf::draining(false) << lf::hasOpsQueued(false) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;

            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 1 );
            ASSERT( c );
        }

        TEST( BalanceNormalTests ,  BalanceDrainingTest ) {
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
            BSONObj limits0 = BSON( sf::maxSize(0LL) << lf::currSize(2LL) << sf::draining(true) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << lf::currSize(0LL) << sf::draining(false) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;

            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            ASSERT( c );
            ASSERT_EQUALS( c->to , "shard1" );
            ASSERT_EQUALS( c->from , "shard0" );
            ASSERT( ! c->chunk.isEmpty() );
        }

        TEST( BalancerPolicyTests , BalanceEndedDrainingTest ) {
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
            BSONObj limits0 = BSON( sf::maxSize(0LL) << lf::currSize(2LL) << sf::draining(false) );
            BSONObj limits1 = BSON( sf::maxSize(0LL) << lf::currSize(0LL) << sf::draining(true) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;

            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            ASSERT( ! c );
        }

        TEST( BalancerPolicyTests , BalanceImpasseTest ) {
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

            // shard0 is draining, shard1 is maxed out, shard2 has writebacks pending
            BalancerPolicy::ShardToLimitsMap limitsMap;
            BSONObj limits0 = BSON( sf::maxSize(0LL) << lf::currSize(2LL) << sf::draining(true) );
            BSONObj limits1 = BSON( sf::maxSize(1LL) << lf::currSize(1LL) << sf::draining(false) );
            BSONObj limits2 = BSON( sf::maxSize(0LL) << lf::currSize(1LL) << lf::hasOpsQueued(true) );
            limitsMap["shard0"] = limits0;
            limitsMap["shard1"] = limits1;
            limitsMap["shard2"] = limits2;

            BalancerPolicy::ChunkInfo* c = NULL;
            c = BalancerPolicy::balance( "ns", limitsMap, chunkMap, 0 );
            ASSERT( ! c );
        }

    }
}
