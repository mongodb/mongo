/*    Copyright 2012 10gen Inc.
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

#include "mongo/base/owned_pointer_map.h"
#include "mongo/platform/random.h"
#include "mongo/s/balancer_policy.h"
#include "mongo/s/config.h"
#include "mongo/s/type_chunk.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    namespace {
        
        typedef OwnedPointerMap<string, OwnedPointerVector<ChunkType> > OwnedShardToChunksMap;

        TEST( BalancerPolicyTests , SizeMaxedShardTest ) {
            ASSERT( ! ShardInfo( 0, 0, false, false ).isSizeMaxed() );
            ASSERT( ! ShardInfo( 100LL, 80LL, false, false ).isSizeMaxed() );
            ASSERT( ShardInfo( 100LL, 110LL, false, false ).isSizeMaxed() );
        }

        TEST( BalancerPolicyTests , BalanceNormalTest  ) {
            // 2 chunks and 0 chunk shards
            OwnedShardToChunksMap chunkMap;
            auto_ptr<OwnedPointerVector<ChunkType> > chunks(new OwnedPointerVector<ChunkType>());

            auto_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
            chunk->setMax(BSON("x" << 49));
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 49));
            chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
            chunks->push_back(chunk.release());

            chunkMap.mutableMap()["shard0"] = chunks.release();
            chunkMap.mutableMap()["shard1"] = new OwnedPointerVector<ChunkType>();

            // no limits
            ShardInfoMap info;
            info["shard0"] = ShardInfo( 0, 2, false, false );
            info["shard1"] = ShardInfo( 0, 0, false, false );

            MigrateInfo* c = NULL;
            DistributionStatus status(info, chunkMap.map());
            c = BalancerPolicy::balance( "ns", status, 1 );
            ASSERT( c );
        }


        TEST( BalancerPolicyTests , BalanceJumbo  ) {
            // 2 chunks and 0 chunk shards
            OwnedShardToChunksMap chunkMap;
            auto_ptr<OwnedPointerVector<ChunkType> > chunks(new OwnedPointerVector<ChunkType>());

            auto_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
            chunk->setMax(BSON("x" << 10));
            chunk->setJumbo(true);
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 10));
            chunk->setMax(BSON("x" << 20));
            chunk->setJumbo(true);
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 20));
            chunk->setMax(BSON("x" << 30));
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 30));
            chunk->setMax(BSON("x" << 40));
            chunk->setJumbo(true);
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 40));
            chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
            chunks->push_back(chunk.release());

            chunkMap.mutableMap()["shard0"] = chunks.release();
            chunkMap.mutableMap()["shard1"] = new OwnedPointerVector<ChunkType>;

            // no limits
            ShardInfoMap info;
            info["shard0"] = ShardInfo( 0, 2, false, false );
            info["shard1"] = ShardInfo( 0, 0, false, false );

            MigrateInfo* c = NULL;
            DistributionStatus status(info, chunkMap.map());
            c = BalancerPolicy::balance( "ns", status, 1 );
            ASSERT( c );
            ASSERT_EQUALS( 30, c->chunk.max["x"].numberInt() );
        }


        TEST( BalanceNormalTests ,  BalanceDrainingTest ) {
            // one normal, one draining
            // 2 chunks and 0 chunk shards
            OwnedShardToChunksMap chunkMap;
            auto_ptr<OwnedPointerVector<ChunkType> > chunks(new OwnedPointerVector<ChunkType>());

            auto_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
            chunk->setMax(BSON("x" << 49));
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 49));
            chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
            chunks->push_back(chunk.release());

            chunkMap.mutableMap()["shard0"] = chunks.release();
            chunkMap.mutableMap()["shard1"] = new OwnedPointerVector<ChunkType>();

            // shard0 is draining
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0LL, 2LL, true, false );
            limitsMap["shard1"] = ShardInfo( 0LL, 0LL, false, false );

            DistributionStatus status(limitsMap, chunkMap.map());
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( c );
            ASSERT_EQUALS( c->to , "shard1" );
            ASSERT_EQUALS( c->from , "shard0" );
            ASSERT( ! c->chunk.min.isEmpty() );
        }

        TEST( BalancerPolicyTests , BalanceEndedDrainingTest ) {
            // 2 chunks and 0 chunk (drain completed) shards
            OwnedShardToChunksMap chunkMap;
            auto_ptr<OwnedPointerVector<ChunkType> > chunks(new OwnedPointerVector<ChunkType>());

            auto_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
            chunk->setMax(BSON("x" << 49));
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 49));
            chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
            chunks->push_back(chunk.release());

            chunkMap.mutableMap()["shard0"] = chunks.release();
            chunkMap.mutableMap()["shard1"] = new OwnedPointerVector<ChunkType>();

            // no limits
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0, 2, false, false );
            limitsMap["shard1"] = ShardInfo( 0, 0, true, false );

            DistributionStatus status(limitsMap, chunkMap.map());
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( ! c );
        }

        TEST( BalancerPolicyTests , BalanceImpasseTest ) {
            // one maxed out, one draining
            // 2 chunks and 0 chunk shards
            OwnedShardToChunksMap chunkMap;
            auto_ptr<OwnedPointerVector<ChunkType> > chunks(new OwnedPointerVector<ChunkType>());

            auto_ptr<ChunkType> chunk(new ChunkType());
            chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
            chunk->setMax(BSON("x" << 49));
            chunks->push_back(chunk.release());

            chunk.reset(new ChunkType());
            chunk->setMin(BSON("x" << 49));
            chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
            chunks->push_back(chunk.release());

            chunkMap.mutableMap()["shard0"] = new OwnedPointerVector<ChunkType>();
            chunkMap.mutableMap()["shard1"] = chunks.release();
            chunkMap.mutableMap()["shard2"] = new OwnedPointerVector<ChunkType>();

            // shard0 is draining, shard1 is maxed out, shard2 has writebacks pending
            ShardInfoMap limitsMap;
            limitsMap["shard0"] = ShardInfo( 0, 2, true, false );
            limitsMap["shard1"] = ShardInfo( 1, 1, false, false );
            limitsMap["shard2"] = ShardInfo( 0, 1, true, false );

            DistributionStatus status(limitsMap, chunkMap.map());
            MigrateInfo* c = BalancerPolicy::balance( "ns", status, 0 );
            ASSERT( ! c );
        }


        void addShard( OwnedShardToChunksMap& map, unsigned numChunks, bool last ) {
            unsigned total = 0;
            const OwnedShardToChunksMap::MapType& shardToChunks = map.map();
            for (OwnedShardToChunksMap::MapType::const_iterator i = shardToChunks.begin();
                    i != shardToChunks.end(); ++i) {
                total += i->second->size();
            }
            
            stringstream ss;
            ss << "shard" << shardToChunks.size();
            string myName = ss.str();
            auto_ptr<OwnedPointerVector<ChunkType> > chunksList(
                    new OwnedPointerVector<ChunkType>());
            
            for ( unsigned i=0; i<numChunks; i++ ) {
                auto_ptr<ChunkType> chunk(new ChunkType());
                
                if ( i == 0 && total == 0 )
                    chunk->setMin(BSON("x" << BSON("$minKey" << 1)));
                else
                    chunk->setMin(BSON("x" << total + i));
                
                if ( last && i == ( numChunks - 1 ) )
                    chunk->setMax(BSON("x" << BSON("$maxKey" << 1)));
                else
                    chunk->setMax(BSON("x" << 1 + total + i));

                chunksList->push_back(chunk.release());
            }

            OwnedShardToChunksMap::MapType& mutableShardToChunks = map.mutableMap();
            mutableShardToChunks[myName] = chunksList.release();
        }

        void moveChunk( OwnedShardToChunksMap& map, MigrateInfo* m ) {
            vector<ChunkType*>& chunks = map.mutableMap()[m->from]->mutableVector();
            for (vector<ChunkType*>::iterator i = chunks.begin(); i != chunks.end(); ++i) {
                if ((*i)->getMin() == m->chunk.min) {
                    map.mutableMap()[m->to]->push_back(*i);
                    chunks.erase(i);
                    return;
                }
            }
            verify(0);
        }

        TEST( BalancerPolicyTests, MultipleDraining ) {
            OwnedShardToChunksMap chunks;
            addShard( chunks, 5 , false );
            addShard( chunks, 10 , false );
            addShard( chunks, 5 , true );

            ShardInfoMap shards;
            shards["shard0"] = ShardInfo( 0, 5, true, false );
            shards["shard1"] = ShardInfo( 0, 5, true, false );
            shards["shard2"] = ShardInfo( 0, 5, false, false );
            
            DistributionStatus d(shards, chunks.map());
            MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
            ASSERT( m );
            ASSERT_EQUALS( "shard2" , m->to );
        }


        TEST( BalancerPolicyTests, TagsDraining ) {

            OwnedShardToChunksMap chunks;
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
                DistributionStatus d(shards, chunks.map());
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

            ASSERT_EQUALS( 7U , chunks.mutableMap()["shard0"]->size() );
            ASSERT_EQUALS( 0U , chunks.mutableMap()["shard1"]->size() );
            ASSERT_EQUALS( 8U , chunks.mutableMap()["shard2"]->size() );
        }


        TEST( BalancerPolicyTests, TagsPolicyChange ) {
            OwnedShardToChunksMap chunks;
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
                
                DistributionStatus d(shards, chunks.map());
                d.addTagRange( TagRange( BSON( "x" << -1 ), BSON( "x" << 1000 ) , "a" ) );
                
                MigrateInfo* m = BalancerPolicy::balance( "ns", d, 0 );
                if ( ! m )
                    break;

                moveChunk( chunks, m );
            }
            
            const size_t shard0Size = chunks.mutableMap()["shard0"]->size();
            const size_t shard1Size = chunks.mutableMap()["shard1"]->size();
            ASSERT_EQUALS( 15U , shard0Size + shard1Size );
            ASSERT(shard0Size == 7U || shard0Size == 8U);
            ASSERT_EQUALS(0U, chunks.mutableMap()["shard2"]->size());

        }


        TEST( BalancerPolicyTests, TagsSelector ) {
            OwnedShardToChunksMap chunks;
            ShardInfoMap shards;
            DistributionStatus d(shards, chunks.map());
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 1 ), BSON( "x" << 10 ) , "a" ) ) );
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 10 ), BSON( "x" << 20 ) , "b" ) ) );
            ASSERT( d.addTagRange( TagRange( BSON( "x" << 20 ), BSON( "x" << 30 ) , "c" ) ) );

            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 20 ), BSON( "x" << 30 ) , "c" ) ) );
            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 22 ), BSON( "x" << 28 ) , "c" ) ) );
            ASSERT( ! d.addTagRange( TagRange( BSON( "x" << 28 ), BSON( "x" << 33 ) , "c" ) ) );

            ChunkType chunk;
            chunk.setMin(BSON("x" << -4));
            ASSERT_EQUALS("", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 0));
            ASSERT_EQUALS("", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 1));
            ASSERT_EQUALS("a", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 10));
            ASSERT_EQUALS("b", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 15));
            ASSERT_EQUALS("b", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 25));
            ASSERT_EQUALS("c", d.getTagForChunk(chunk));

            chunk.setMin(BSON("x" << 35));
            ASSERT_EQUALS("", d.getTagForChunk(chunk));
        }

        /**
         * Idea for this test is to set up three shards, one of which is overloaded (too much data).
         *
         * Even though the overloaded shard has less chunks, we shouldn't move chunks to that shard.
         */
        TEST( BalancerPolicyTests, MaxSizeRespect ) {

            OwnedShardToChunksMap chunks;
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

            DistributionStatus d(shards, chunks.map());
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

            OwnedShardToChunksMap chunks;
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

            DistributionStatus d(shards, chunks.map());
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

                OwnedShardToChunksMap chunks;
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

                    DistributionStatus d(shards, chunks.map());
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
