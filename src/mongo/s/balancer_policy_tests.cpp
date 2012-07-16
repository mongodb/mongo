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
#include "mongo/util/concurrency/remap_lock.h"
#include "mongo/s/config.h"
#include "mongo/s/balancer_policy.h"

namespace mongo {

    // these are all crutch and hopefully will eventually go away
    CmdLine cmdLine;
    bool inShutdown() { return false; }
    void setupSignals( bool inFork ) {}
    DBClientBase *createDirectClient() { return 0; }
    void dbexit( ExitCode rc, const char *why ){
        log()  << "dbexit called? :(" << endl;
        ::_exit(-1);
    }
    bool haveLocalShardingInfo( const string& ns ) {
        return false;
    }

    RemapLock::RemapLock() {}
    RemapLock::~RemapLock() {}

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
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
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

        TEST( BalanceNormalTests ,  BalanceDrainingTest ) {
            // one normal, one draining
            // 2 chunks and 0 chunk shards
            ShardToChunksMap chunkMap;
            vector<BSONObj> chunks;
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
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
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
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
            chunks.push_back(BSON( "min" << BSON( "x" << BSON( "$minKey"<<1) ) <<
                                   "max" << BSON( "x" << 49 )));
            chunkMap["shard0"] = chunks;
            chunks.clear();
            chunks.push_back(BSON( "min" << BSON( "x" << 49 ) <<
                                   "max" << BSON( "x" << BSON( "$maxkey"<<1 ))));
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
                
                chunks.push_back( BSON( "min" << min << "max" << max ) );
            }

        }

        void moveChunk( ShardToChunksMap& map, MigrateInfo* m ) {
            vector<BSONObj>& chunks = map[m->from];
            for ( vector<BSONObj>::iterator i = chunks.begin(); i != chunks.end(); ++i ) {
                if ( i->getField("min").Obj() == m->chunk.min ) {
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

            ASSERT_EQUALS( "" , d.getTagForChunk( BSON( "min" << BSON( "x" << -4 ) ) ) );
            ASSERT_EQUALS( "" , d.getTagForChunk( BSON( "min" << BSON( "x" << 0 ) ) ) );
            ASSERT_EQUALS( "a" , d.getTagForChunk( BSON( "min" << BSON( "x" << 1 ) ) ) );
            ASSERT_EQUALS( "b" , d.getTagForChunk( BSON( "min" << BSON( "x" << 10 ) ) ) );
            ASSERT_EQUALS( "b" , d.getTagForChunk( BSON( "min" << BSON( "x" << 15 ) ) ) );
            ASSERT_EQUALS( "c" , d.getTagForChunk( BSON( "min" << BSON( "x" << 25 ) ) ) );
            ASSERT_EQUALS( "" , d.getTagForChunk( BSON( "min" << BSON( "x" << 35 ) ) ) );


        }


    }
}
