// balancer_test.cpp : balance/balancer_policy.{h,cpp} unit tests

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

#include "../pch.h"

#include "dbtests.h"
#include "../s/balancer_policy.h"

namespace mongo {

    using mongo::BalancerPolicy;

    class MaxSizeForShard{
    public:
        void run(){
            BSONObj shard0 = BSON( "maxSize" << 0LL << "currSize" << 0LL );
            BSONObj shard1 = BSON( "maxSize" << 100LL << "currSize" << 80LL );
            BSONObj shard2 = BSON( "maxSize" << 100LL << "currSize" << 110LL );
            map< string, BSONObj > shardLimitsMap; 
            shardLimitsMap["shard0"] = shard0;
            shardLimitsMap["shard1"] = shard1;
            shardLimitsMap["shard2"] = shard2;

            ASSERT( BalancerPolicy::isReceiver( "shard0", shardLimitsMap ) );
            ASSERT( BalancerPolicy::isReceiver( "shard1", shardLimitsMap ) );
            ASSERT( ! BalancerPolicy::isReceiver( "shard2", shardLimitsMap ) );
        }
    };

    class BalancerSuite : public Suite {
    public:
        BalancerSuite() : Suite( "balancer" ){}

        void setupTests(){
            add< MaxSizeForShard >();
            // TODO: complete the test suite
        } 
    } BalancerSuite;

}  // mongo namespace
