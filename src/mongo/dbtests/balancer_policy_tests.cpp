// @file balancer_policy_test.cpp

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
#include "dbtests.h"

// TODO SERVER-1822
//#include "../s/config.h" // for ShardFields
//#include "../s/balancer_policy.h"

namespace BalancerPolicyTests {

//
// TODO SERVER-1822
//
#if 0

    typedef mongo::ShardFields sf;  // fields from 'shards' colleciton
    typedef mongo::LimitsFields lf; // fields from the balancer's limits map

    class SizeMaxedShardTest {
    public:
        void run() {
            BSONObj shard0 = BSON( sf::maxSize(0LL) << lf::currSize(0LL) );
            ASSERT( ! BalancerPolicy::isSizeMaxed( shard0 ) );

            BSONObj shard1 = BSON( sf::maxSize(100LL) << lf::currSize(80LL) );
            ASSERT( ! BalancerPolicy::isSizeMaxed( shard1 ) );

            BSONObj shard2 = BSON( sf::maxSize(100LL) << lf::currSize(110LL) );
            ASSERT( BalancerPolicy::isSizeMaxed( shard2 ) );

            BSONObj empty;
            ASSERT( ! BalancerPolicy::isSizeMaxed( empty ) );
        }
    };

    class DrainingShardTest {
    public:
        void run() {
            BSONObj shard0 = BSON( sf::draining(true) );
            ASSERT( BalancerPolicy::isDraining( shard0 ) );

            BSONObj shard1 = BSON( sf::draining(false) );
            ASSERT( ! BalancerPolicy::isDraining( shard1 ) );

            BSONObj empty;
            ASSERT( ! BalancerPolicy::isDraining( empty ) );
        }
    };

    class BalanceNormalTest {
    public:
        void run() {
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
    };

    class BalanceDrainingTest {
    public:
        void run() {
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
    };

    class BalanceEndedDrainingTest {
    public:
        void run() {
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
    };

    class BalanceImpasseTest {
    public:
        void run() {
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
    };

//
// TODO SERVER-1822
//
#endif // #if 0

    class All : public Suite {
    public:
        All() : Suite( "balancer_policy" ) {
        }

        void setupTests() {
            // TODO SERVER-1822
            // add< SizeMaxedShardTest >();
            // add< DrainingShardTest >();
            // add< BalanceNormalTest >();
            // add< BalanceDrainingTest >();
            // add< BalanceEndedDrainingTest >();
            // add< BalanceImpasseTest >();
        }
    } allTests;

} // namespace BalancerPolicyTests
