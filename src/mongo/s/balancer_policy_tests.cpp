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

#include "mongo/platform/random.h"
#include "mongo/s/balancer_policy.h"
#include "mongo/s/config.h"
#include "mongo/s/type_chunk.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // these are all crutch and hopefully will eventually go away
    CmdLine cmdLine;
    bool inShutdown() { return false; }
    DBClientBase *createDirectClient() { return 0; }
    void dbexit( ExitCode rc, const char *why ){
        log()  << "dbexit called? :(" << endl;
        ::_exit(-1);
    }
    bool haveLocalShardingInfo( const string& ns ) {
        return false;
    }

    namespace {
        
        TEST( BalancerPolicyTests , SizeMaxedShardTest ) {
            ASSERT( ! ShardInfo( 0, 0, false, false ).isSizeMaxed() );
            ASSERT( ! ShardInfo( 100LL, 80LL, false, false ).isSizeMaxed() );
            ASSERT( ShardInfo( 100LL, 110LL, false, false ).isSizeMaxed() );
        }

        TEST( BalancerPolicyTests , BalanceNormalTest  ) {
            // 2 chunks and 0 chunk shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON(ChunkType::min(BSON("x" << BSON("$minKey"<<1))) <<
                                  ChunkType::max(BSON("x" << 49))));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 49)) <<
                                  ChunkType::max(BSON("x" << BSON("$maxkey"<<1)))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunkMap["shard1"] = chunks;

            // no limits
            ShardInfoMap info;
            info["shard0"] = ShardInfo( 0, 2, false, false );
            info["shard1"] = ShardInfo( 0, 0, false, false );

            MigrateInfo* c = NULL;
            DistributionStatus status( info, chunkMap );
            c = BalancerPolicy::balance( "ns", status, 1 );
            ASSERT( c );
        }


        TEST( BalancerPolicyTests , BalanceJumbo  ) {
            // 2 chunks and 0 chunk shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON(ChunkType::min(BSON("x" << BSON("$minKey"<<1))) <<
                                  ChunkType::max(BSON("x" << 10)) <<
                                  ChunkType::jumbo(true)));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 10)) <<
                                  ChunkType::max(BSON("x" << 20)) <<
                                  ChunkType::jumbo(true)));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 20)) <<
                                  ChunkType::max(BSON("x" << 30))));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 30)) <<
                                  ChunkType::max(BSON("x" << 40)) <<
                                  ChunkType::jumbo(true)));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 40)) <<
                                  ChunkType::max(BSON("x" << BSON("$maxkey"<<1))) <<
                                  ChunkType::jumbo(true)));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunkMap["shard1"] = chunks;

            // no limits
            ShardInfoMap info;
            info["shard0"] = ShardInfo( 0, 2, false, false );
            info["shard1"] = ShardInfo( 0, 0, false, false );

            MigrateInfo* c = NULL;
            DistributionStatus status( info, chunkMap );
            c = BalancerPolicy::balance( "ns", status, 1 );
            ASSERT( c );
            ASSERT_EQUALS( 30, c->chunk.max["x"].numberInt() );
        }


        TEST( BalanceNormalTests ,  BalanceDrainingTest ) {
            // one normal, one draining
            // 2 chunks and 0 chunk shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON(ChunkType::min(BSON("x" << BSON("$minKey"<<1))) <<
                                  ChunkType::max(BSON("x" << 49))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 49))<<
                                  ChunkType::max(BSON("x" << BSON("$maxkey"<<1)))));
            chunkMap["shard1"] = chunks;

            // shard0 is draining
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0LL, 2LL, true, false );
            limitsMap["shard1"] = ShardInfo( 0LL, 0LL, false, false );

            DistributionStatus status( limitsMap, chunkMap );
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( c );
            ASSERT_EQUALS( c->to , "shard1" );
            ASSERT_EQUALS( c->from , "shard0" );
            ASSERT( ! c->chunk.min.isEmpty() );
        }

        TEST( BalancerPolicyTests , BalanceEndedDrainingTest ) {
            // 2 chunks and 0 chunk (drain completed) shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON(ChunkType::min(BSON("x" << BSON("$minKey"<<1))) <<
                                  ChunkType::max(BSON("x" << 49))));
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 49))<<
                                  ChunkType::max(BSON("x" << BSON("$maxkey"<<1)))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunkMap["shard1"] = chunks;

            // no limits
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0, 2, false, false );
            limitsMap["shard1"] = ShardInfo( 0, 0, true, false );

            DistributionStatus status( limitsMap, chunkMap );
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( ! c );
        }

        TEST( BalancerPolicyTests , BalanceImpasseTest ) {
            // one maxed out, one draining
            // 2 chunks and 0 chunk shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON(ChunkType::min(BSON("x" << BSON("$minKey"<<1))) <<
                                  ChunkType::max(BSON("x" << 49))));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON(ChunkType::min(BSON("x" << 49)) <<
                                  ChunkType::max(BSON("x" << BSON("$maxkey"<<1)))));
            chunkMap["shard1"] = chunks;

            // shard0 is draining, shard1 is maxed out, shard2 has writebacks pending
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0, 2, true, false );
            limitsMap["shard1"] = ShardInfo( 1, 1, false, false );
            limitsMap["shard2"] = ShardInfo( 0, 1, true, false );

            DistributionStatus status(limitsMap, chunkMap);
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( ! c );
        }


        void addShard( ShardToChunksMap& map, unsigned numChunks, bool last ) {
            unsigned total = 0;
            for ( ShardToChunksMap::const_iterator i = map.begin(); i != map.end(); ++i ) 
                total += i->second.size();
            
            stringstream ss;
            ss << "shard" << map.size();
            string myName = ss.str();
            vector<BSONObj>& chunks = map[myName];
            
            for ( unsigned i=0; i<numChunks; i++ ) {
                BSONObj min,max;
                
                if ( i == 0 && total == 0 )
                    min = BSON( "x" << BSON( "$minKey" << 1 ) );
                else
                    min = BSON( "x" << total + i );
                
                if ( last && i == ( numChunks - 1 ) )
                    max = BSON( "x" << BSON( "$maxKey" << 1 ) );
                else
                    max = BSON( "x" << 1 + total + i );

                chunks.push_back( BSON(ChunkType::min(min) << ChunkType::max(max)));
            }

        }

        void moveChunk( ShardToChunksMap& map, MigrateInfo* m ) {
            vector<BSONObj>& chunks = map[m->from];
            for ( vector<BSONObj>::iterator i = chunks.begin(); i != chunks.end(); ++i ) {
                if (i->getField(ChunkType::min()).Obj() == m->chunk.min) {
                    map[m->to].push_back( *i );
                    chunks.erase( i );
                    return;
                }
            }
            verify(0);
        }

        TEST( BalancerPolicyTests, MultipleDraining ) {
            ShardToChunksMap chunks;
            addShard( chunks, 5 , false );
            addShard( chunks, 10 , false );
            addShard( chunks, 5 , true );

            ShardInfoMap shards;
            shards["shard0"] = ShardInfo( 0, 5, true, false );
            shards["shard1"] = ShardInfo( 0, 5, true, false );
            shards["shard2"] = ShardInfo( 0, 5, false, false );
            
            DistributionStatus d( shards, chunks );
            MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
            ASSERT( m );
            ASSERT_EQUALS( "shard2" , m->to );
        }


        TEST( BalancerPolicyTests, TagsDraining ) {

            ShardToChunksMap chunks;
            addShard( chunks, 5 , false );
            addShard( chunks, 5 , false );
            addShard( chunks, 5 , true );
            
            ShardInfoMap shards;
            shards["shard0"] = ShardInfo( 0, 5, false, false );
            shards["shard1"] = ShardInfo( 0, 5, true, false );
            shards["shard2"] = ShardInfo( 0, 5, false, false );
            
            shards["shard0"].addTag( "a" );
            shards["shard1"].addTag( "a" );
            shards["shard1"].addTag( "b" );
            shards["shard2"].addTag( "b" );
            
            while ( true ) {
                DistributionStatus d( shards, chunks );
                d.addTagRange( TagRange( BSON( "x" << -1 ), BSON( "x" << 7 ) , "a" ) );
                d.addTagRange( TagRange( BSON( "x" << 7 ), BSON( "x" << 1000 ) , "b" ) );
                
                MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
                if ( ! m ) 
                    break;

                if ( m->chunk.min["x"].numberInt() < 7 )
                    ASSERT_EQUALS( "shard0" , m->to );
                else
                    ASSERT_EQUALS( "shard2" , m->to );

                moveChunk( chunks, m );
            }

            ASSERT_EQUALS( 7U , chunks["shard0"].size() );
            ASSERT_EQUALS( 0U , chunks["shard1"].size() );
            ASSERT_EQUALS( 8U , chunks["shard2"].size() );
        }


        TEST( BalancerPolicyTests, TagsPolicyChange ) {
            ShardToChunksMap chunks;
            addShard( chunks, 5 , false );
            addShard( chunks, 5 , false );
            addShard( chunks, 5 , true );
            
            ShardInfoMap shards;
            shards["shard0"] = ShardInfo( 0, 5, false, false );
            shards["shard1"] = ShardInfo( 0, 5, false, false );
            shards["shard2"] = ShardInfo( 0, 5, false, false );
            
            shards["shard0"].addTag( "a" );
            shards["shard1"].addTag( "a" );
            
            while ( true ) {
                
                DistributionStatus d( shards, chunks );
                d.addTagRange( TagRange( BSON( "x" << -1 ), BSON( "x" << 1000 ) , "a" ) );
                
                MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
                if ( ! m )
                    break;

                moveChunk( chunks, m );
            }
            
            ASSERT_EQUALS( 15U , chunks["shard0"].size() + chunks["shard1"].size() );
            ASSERT( chunks["shard0"].size() == 7U || chunks["shard0"].size() == 8U );
            ASSERT_EQUALS( 0U , chunks["shard2"].size() );

        }


        TEST( BalancerPolicyTests, TagsSelector ) {
            ShardToChunksMap chunks;
            ShardInfoMap shards;
            DistributionStatus d( shards, chunks );
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 1 ), BSON( "x" << 10 ) , "a" ) ) );
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 10 ), BSON( "x" << 20 ) , "b" ) ) );
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 20 ), BSON( "x" << 30 ) , "c" ) ) );

            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 20 ), BSON( "x" << 30 ) , "c" ) ) );
            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 22 ), BSON( "x" << 28 ) , "c" ) ) );
            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 28 ), BSON( "x" << 33 ) , "c" ) ) );

            ASSERT_EQUALS("", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << -4)))));
            ASSERT_EQUALS("", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 0)))));
            ASSERT_EQUALS("a", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 1)))));
            ASSERT_EQUALS("b", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 10)))));
            ASSERT_EQUALS("b", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 15)))));
            ASSERT_EQUALS("c", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 25)))));
            ASSERT_EQUALS("", d.getTagForChunk(BSON(ChunkType::min(BSON("x" << 35)))));
        }

        /**
         * Idea for this test is to set up three shards, one of which is overloaded (too much data).
         *
         * Even though the overloaded shard has less chunks, we shouldn't move chunks to that shard.
         */
        TEST( BalancerPolicyTests, MaxSizeRespect ) {

            ShardToChunksMap chunks;
            addShard( chunks, 3 , false );
            addShard( chunks, 4 , false );
            addShard( chunks, 6 , true );

            // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 3.
            // Other shards have maxSize = 0 = unset.

            ShardInfoMap shards;
            // ShardInfo(maxSize, currSize, draining, opsQueued)
            shards["shard0"] = ShardInfo( 1, 3, false, false );
            shards["shard1"] = ShardInfo( 0, 4, false, false );
            shards["shard2"] = ShardInfo( 0, 6, false, false );

            DistributionStatus d( shards, chunks );
            MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
            ASSERT( m );
            ASSERT_EQUALS( "shard2" , m->from );
            ASSERT_EQUALS( "shard1" , m->to );

        }

        /**
         * Here we check that being over the maxSize is *not* equivalent to draining, we don't want
         * to empty shards for no other reason than they are over this limit.
         */
        TEST( BalancerPolicyTests, MaxSizeNoDrain ) {

            ShardToChunksMap chunks;
            // Shard0 will be overloaded
            addShard( chunks, 4 , false );
            addShard( chunks, 4 , false );
            addShard( chunks, 4 , true );

            // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 4.
            // Other shards have maxSize = 0 = unset.

            ShardInfoMap shards;
            // ShardInfo(maxSize, currSize, draining, opsQueued)
            shards["shard0"] = ShardInfo( 1, 4, false, false );
            shards["shard1"] = ShardInfo( 0, 4, false, false );
            shards["shard2"] = ShardInfo( 0, 4, false, false );

            DistributionStatus d( shards, chunks );
            MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
            ASSERT( !m );
        }

        /**
         * Idea behind this test is that we set up several shards, the first two of which are
         * draining and the second two of which have a data size limit.  We also simulate a random
         * number of chunks on each shard.
         *
         * Once the shards are setup, we virtually migrate numChunks times, or until there are no
         * more migrations to run.  Each chunk is assumed to have a size of 1 unit, and we increment
         * our currSize for each shard as the chunks move.
         *
         * Finally, we ensure that the drained shards are drained, the data-limited shards aren't
         * overloaded, and that all shards (including the data limited shard if the baseline isn't
         * over the limit are balanced to within 1 unit of some baseline.
         *
         */
        TEST( BalancerPolicyTests, Simulation ) {

            // Hardcode seed here, make test deterministic.
            int64_t seed = 1337;
            PseudoRandom rng(seed);

            // Run test 10 times
            for (int test = 0; test < 10; test++) {

                //
                // Setup our shards as draining, with maxSize, and normal
                //

                int numShards = 7;
                int numChunks = 0;

                ShardToChunksMap chunks;
                ShardInfoMap shards;

                map<string,int> expected;

                for (int i = 0; i < numShards; i++) {

                    int numShardChunks = rng.nextInt32(100);
                    bool draining = i < 2;
                    bool maxed = i >= 2 && i < 4;

                    if (draining) expected[str::stream() << "shard" << i] = 0;
                    if (maxed) expected[str::stream() << "shard" << i] = numShardChunks + 1;

                    addShard(chunks, numShardChunks, false);
                    numChunks += numShardChunks;

                    shards[str::stream() << "shard" << i] =
                            ShardInfo(maxed ? numShardChunks + 1 : 0,
                                              numShardChunks, draining, false);
                }

                for (ShardInfoMap::iterator it = shards.begin(); it != shards.end(); ++it) {
                    log() << it->first << " : " << it->second.toString() << endl;
                }

                //
                // Perform migrations and increment data size as chunks move
                //

                for (int i = 0; i < numChunks; i++) {

                    DistributionStatus d( shards, chunks );
                    MigrateInfo* m = BalancerPolicy::balance( "ns", d, i != 0 );

                    if (!m) {
                        log() << "Finished with test moves." << endl;
                        break;
                    }

                    moveChunk(chunks, m);

                    {
                        ShardInfo& info = shards[m->from];
                        shards[m->from] = ShardInfo(info.getMaxSize(),
                                                    info.getCurrSize() - 1,
                                                    info.isDraining(),
                                                    info.hasOpsQueued());
                    }

                    {
                        ShardInfo& info = shards[m->to];
                        shards[m->to] = ShardInfo(info.getMaxSize(),
                                                  info.getCurrSize() + 1,
                                                  info.isDraining(),
                                                  info.hasOpsQueued());
                    }
                }

                //
                // Make sure our balance is correct and our data size is low.
                //

                // The balanced value is the count on the last shard, since it's not draining or
                // limited
                int balancedSize = (--shards.end())->second.getCurrSize();

                for (ShardInfoMap::iterator it = shards.begin(); it != shards.end(); ++it) {
                    log() << it->first << " : " << it->second.toString() << endl;
                }

                for (ShardInfoMap::iterator it = shards.begin(); it != shards.end(); ++it) {

                    log() << it->first << " : " << it->second.toString() << endl;

                    map<string,int>::iterator expectedIt = expected.find(it->first);

                    if (expectedIt == expected.end()) {
                        bool isInRange = it->second.getCurrSize() >= balancedSize - 1 &&
                                         it->second.getCurrSize() <= balancedSize + 1;

                        if (!isInRange) {
                            warning() << "non-limited and non-draining shard had "
                                      << it->second.getCurrSize() << " chunks, expected near "
                                      << balancedSize << endl;
                        }

                        ASSERT(isInRange);
                    }
                    else {
                        int expectedSize = expectedIt->second;
                        bool isInRange = it->second.getCurrSize() <= expectedSize;
                        if (isInRange && expectedSize >= balancedSize) {
                            isInRange = it->second.getCurrSize() >= balancedSize - 1 &&
                                        it->second.getCurrSize() <= balancedSize + 1;
                        }

                        if (!isInRange) {
                            warning() << "limited or draining shard had "
                                      << it->second.getCurrSize() << " chunks, expected less than "
                                      << expectedSize << " and (if less than expected) near "
                                      << balancedSize << endl;
                        }

                        ASSERT(isInRange);
                    }
                }
            }
        }
    }
}
