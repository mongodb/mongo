//@file d_chunk_manager_tests.cpp : s/d_chunk_manager.{h,cpp} tests

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

#include "mongo/dbtests/dbtests.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_chunk_manager.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"

namespace {

    class BasicTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1)) <<
                                      CollectionType::unique(false));

            // single-chunk collection
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(BSON("a" << MINKEY)) <<
                                               ChunkType::max(BSON("a" << MAXKEY))));

            ShardChunkManager s ( collection , chunks );

            BSONObj k1 = BSON( "a" << MINKEY );
            ASSERT( s.keyBelongsToMe( k1 ) );
            BSONObj k2 = BSON( "a" << MAXKEY );
            ASSERT( ! s.keyBelongsToMe( k2 ) );
            BSONObj k3 = BSON( "a" << 1 << "b" << 2 );
            ASSERT( s.keyBelongsToMe( k3 ) );
        }
    };

    class BasicCompoundTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // single-chunk collection
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKeyb_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(BSON("a" << MINKEY <<
                                                                     "b" << MINKEY)) <<
                                               ChunkType::max(BSON("a" << MAXKEY <<
                                                                     "b" << MINKEY))));

            ShardChunkManager s ( collection , chunks );

            BSONObj k1 = BSON( "a" << MINKEY << "b" << MINKEY );
            ASSERT( s.keyBelongsToMe( k1 ) );
            BSONObj k2 = BSON( "a" << MAXKEY << "b" << MAXKEY );
            ASSERT( ! s.keyBelongsToMe( k2 ) );
            BSONObj k3 = BSON( "a" << MINKEY << "b" << 10 );
            ASSERT( s.keyBelongsToMe( k3 ) );
            BSONObj k4 = BSON( "a" << 10 << "b" << 20 );
            ASSERT( s.keyBelongsToMe( k4 ) );
        }
    };

    class RangeTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("x.y") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1)) <<
                                      CollectionType::unique(false));

            // 3-chunk collection, 2 of them being contiguous
            // [min->10) , [10->20) , <gap> , [30->max)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("x.y-a_MinKey") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << MINKEY)) <<
                                               ChunkType::max(BSON("a" << 10))) <<

                                          BSON(ChunkType::name("x.y-a_10") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 10)) <<
                                               ChunkType::max(BSON("a" << 20))) <<

                                          BSON(ChunkType::name("x.y-a_30") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 30)) <<
                                               ChunkType::max(BSON("a" << MAXKEY))));

            ShardChunkManager s ( collection , chunks );

            BSONObj k1 = BSON( "a" << 5 );
            ASSERT( s.keyBelongsToMe( k1 ) );
            BSONObj k2 = BSON( "a" << 10 );
            ASSERT( s.keyBelongsToMe( k2 ) );
            BSONObj k3 = BSON( "a" << 25 );
            ASSERT( ! s.keyBelongsToMe( k3 ) );
            BSONObj k4 = BSON( "a" << 30 );
            ASSERT( s.keyBelongsToMe( k4 ) );
            BSONObj k5 = BSON( "a" << 40 );
            ASSERT( s.keyBelongsToMe( k5 ) );
        }
    };

    class GetNextTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("x.y") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1)) <<
                                      CollectionType::unique(false));

            // empty collection
            BSONArray chunks1 = BSONArray();
            ShardChunkManager s1( collection , chunks1 );

            BSONObj empty;
            BSONObj arbitraryKey = BSON( "a" << 10 );
            BSONObj foundMin, foundMax;

            ASSERT( s1.getNextChunk( empty , &foundMin , &foundMax ) );
            ASSERT( foundMin.isEmpty() );
            ASSERT( foundMax.isEmpty() );
            ASSERT( s1.getNextChunk( arbitraryKey , &foundMin , &foundMax ) );
            ASSERT( foundMin.isEmpty() );
            ASSERT( foundMax.isEmpty() );

            // single-chunk collection
            // [10->20]
            BSONObj key_a10 = BSON( "a" << 10 );
            BSONObj key_a20 = BSON( "a" << 20 );
            BSONArray chunks2 = BSON_ARRAY(BSON(ChunkType::name("x.y-a_10") <<
                                                ChunkType::ns("x.y") <<
                                                ChunkType::min(key_a10) <<
                                                ChunkType::max(key_a20)));
            ShardChunkManager s2( collection , chunks2 );
            ASSERT( s2.getNextChunk( empty , &foundMin , &foundMax ) );
            ASSERT( foundMin.woCompare( key_a10 ) == 0 );
            ASSERT( foundMax.woCompare( key_a20 ) == 0 );

            // 3-chunk collection, 2 of them being contiguous
            // [min->10) , [10->20) , <gap> , [30->max)
            BSONObj key_a30 = BSON( "a" << 30 );
            BSONObj key_min = BSON( "a" << MINKEY );
            BSONObj key_max = BSON( "a" << MAXKEY );
            BSONArray chunks3 = BSON_ARRAY(BSON(ChunkType::name("x.y-a_MinKey") <<
                                                ChunkType::ns("x.y") <<
                                                ChunkType::min(key_min) <<
                                                ChunkType::max(key_a10)) <<
                                           BSON(ChunkType::name("x.y-a_10") <<
                                                ChunkType::ns("x.y") <<
                                                ChunkType::min(key_a10)  <<
                                                ChunkType::max(key_a20)) <<
                                           BSON(ChunkType::name("x.y-a_30") <<
                                                ChunkType::ns("x.y") <<
                                                ChunkType::min(key_a30)  <<
                                                ChunkType::max(key_max)));
            ShardChunkManager s3( collection , chunks3 );
            ASSERT( ! s3.getNextChunk( empty , &foundMin , &foundMax ) ); // not eof
            ASSERT( foundMin.woCompare( key_min ) == 0 );
            ASSERT( foundMax.woCompare( key_a10 ) == 0 );
            ASSERT( ! s3.getNextChunk( key_a10 , &foundMin , &foundMax ) );
            ASSERT( foundMin.woCompare( key_a30 ) == 0 );
            ASSERT( foundMax.woCompare( key_max ) == 0 );
            ASSERT( s3.getNextChunk( key_a30 , &foundMin , &foundMax ) );
        }
    };

    class DeletedTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(true));

            BSONArray chunks = BSONArray();

            ASSERT_THROWS( ShardChunkManager s ( collection , chunks ) , UserException );
        }
    };

    class ClonePlusTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 1-chunk collection
            // [10,0-20,0)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(BSON("a" << 10 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 20 << "b" << 0))));

            ShardChunkManager s ( collection , chunks );

            // new chunk [20,0-30,0)
            BSONObj min = BSON( "a" << 20 << "b" << 0 );
            BSONObj max = BSON( "a" << 30 << "b" << 0 );
            ShardChunkManagerPtr cloned( s.clonePlus( min , max , ChunkVersion( 1, 0, OID() ) /* TODO test version */ ) );

            BSONObj k1 = BSON( "a" << 5 << "b" << 0 );
            ASSERT( ! cloned->keyBelongsToMe( k1 ) );
            BSONObj k2 = BSON( "a" << 20 << "b" << 0 );
            ASSERT( cloned->keyBelongsToMe( k2 ) );
            BSONObj k3 = BSON( "a" << 25 << "b" << 0 );
            ASSERT( cloned->keyBelongsToMe( k3 ) );
            BSONObj k4 = BSON( "a" << 30 << "b" << 0 );
            ASSERT( ! cloned->keyBelongsToMe( k4 ) );
        }
    };

    class ClonePlusExceptionTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 1-chunk collection
            // [10,0-20,0)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(BSON("a" << 10 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 20 << "b" << 0))));

            ShardChunkManager s ( collection , chunks );

            // [15,0-25,0) overlaps [10,0-20,0)
            BSONObj min = BSON( "a" << 15 << "b" << 0 );
            BSONObj max = BSON( "a" << 25 << "b" << 0 );
            ASSERT_THROWS( s.clonePlus ( min , max , ChunkVersion( 1, 0, OID() ) /* TODO test version */ ) , UserException );
        }
    };

    class CloneMinusTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("x.y") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 2-chunk collection
            // [10,0->20,0) , <gap> , [30,0->40,0)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("x.y-a_10b_0") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 10 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 20 << "b" << 0))) <<
                                          BSON(ChunkType::name("x.y-a_30b_0") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 30 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 40 << "b" << 0))));

            ShardChunkManager s ( collection , chunks );

            // deleting chunk [10,0-20,0)
            BSONObj min = BSON( "a" << 10 << "b" << 0 );
            BSONObj max = BSON( "a" << 20 << "b" << 0 );
            ShardChunkManagerPtr cloned( s.cloneMinus( min , max , ChunkVersion( 1, 0, OID() ) /* TODO test version */ ) );

            BSONObj k1 = BSON( "a" << 5 << "b" << 0 );
            ASSERT( ! cloned->keyBelongsToMe( k1 ) );
            BSONObj k2 = BSON( "a" << 15 << "b" << 0 );
            ASSERT( ! cloned->keyBelongsToMe( k2 ) );
            BSONObj k3 = BSON( "a" << 30 << "b" << 0 );
            ASSERT( cloned->keyBelongsToMe( k3 ) );
            BSONObj k4 = BSON( "a" << 35 << "b" << 0 );
            ASSERT( cloned->keyBelongsToMe( k4 ) );
            BSONObj k5 = BSON( "a" << 40 << "b" << 0 );
            ASSERT( ! cloned->keyBelongsToMe( k5 ) );
        }
    };

    class CloneMinusExceptionTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("x.y") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 2-chunk collection
            // [10,0->20,0) , <gap> , [30,0->40,0)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("x.y-a_10b_0") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 10 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 20 << "b" << 0))) <<
                                          BSON(ChunkType::name("x.y-a_30b_0") <<
                                               ChunkType::ns("x.y") <<
                                               ChunkType::min(BSON("a" << 30 << "b" << 0)) <<
                                               ChunkType::max(BSON("a" << 40 << "b" << 0))));

            ShardChunkManager s ( collection , chunks );

            // deleting non-existing chunk [25,0-28,0)
            BSONObj min1 = BSON( "a" << 25 << "b" << 0 );
            BSONObj max1 = BSON( "a" << 28 << "b" << 0 );
            ASSERT_THROWS( s.cloneMinus( min1 , max1 , ChunkVersion( 1, 0, OID() ) /* TODO test version */ ) , UserException );


            // deletin an overlapping range (not exactly a chunk) [15,0-25,0)
            BSONObj min2 = BSON( "a" << 15 << "b" << 0 );
            BSONObj max2 = BSON( "a" << 25 << "b" << 0 );
            ASSERT_THROWS( s.cloneMinus( min2 , max2 , ChunkVersion( 1, 0, OID() ) /* TODO test version */ ) , UserException );
        }
    };

    class CloneSplitTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 1-chunk collection
            // [10,0-20,0)
            BSONObj min = BSON( "a" << 10 << "b" << 0 );
            BSONObj max = BSON( "a" << 20 << "b" << 0 );
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(min) <<
                                               ChunkType::max(max)));

            ShardChunkManager s ( collection , chunks );

            BSONObj split1 = BSON( "a" << 15 << "b" << 0 );
            BSONObj split2 = BSON( "a" << 18 << "b" << 0 );
            vector<BSONObj> splitKeys;
            splitKeys.push_back( split1 );
            splitKeys.push_back( split2 );
            ChunkVersion version( 1 , 99, OID() ); // first chunk 1|99 , second 1|100
            ShardChunkManagerPtr cloned( s.cloneSplit( min , max , splitKeys , version ) );

            version.incMinor(); /* second chunk 1|100, first split point */
            version.incMinor(); /* third chunk 1|101, second split point */
            ASSERT_EQUALS( cloned->getVersion().toLong() , version.toLong() /* 1|101 */ );
            ASSERT_EQUALS( s.getNumChunks() , 1u );
            ASSERT_EQUALS( cloned->getNumChunks() , 3u );
            ASSERT( cloned->keyBelongsToMe( min ) );
            ASSERT( cloned->keyBelongsToMe( split1 ) );
            ASSERT( cloned->keyBelongsToMe( split2 ) );
            ASSERT( ! cloned->keyBelongsToMe( max ) );
        }
    };

    class CloneSplitExceptionTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1 << "b" << 1)) <<
                                      CollectionType::unique(false));

            // 1-chunk collection
            // [10,0-20,0)
            BSONObj min = BSON( "a" << 10 << "b" << 0 );
            BSONObj max = BSON( "a" << 20 << "b" << 0 );
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_MinKey") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(min) <<
                                               ChunkType::max(max)));

            ShardChunkManager s ( collection , chunks );

            BSONObj badSplit = BSON( "a" << 5 << "b" << 0 );
            vector<BSONObj> splitKeys;
            splitKeys.push_back( badSplit );
            ASSERT_THROWS( s.cloneSplit( min , max , splitKeys , ChunkVersion( 1, 0, OID() ) ) , UserException );

            BSONObj badMax = BSON( "a" << 25 << "b" << 0 );
            BSONObj split = BSON( "a" << 15 << "b" << 0 );
            splitKeys.clear();
            splitKeys.push_back( split );
            ASSERT_THROWS( s.cloneSplit( min , badMax, splitKeys , ChunkVersion( 1, 0, OID() ) ) , UserException );
        }
    };

    class EmptyShardTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1)) <<
                                      CollectionType::unique(false));

            // no chunks on this shard
            BSONArray chunks;

            // shard can have zero chunks for an existing collection
            // version should be 0, though
            ShardChunkManager s( collection , chunks );
            ASSERT_EQUALS( s.getVersion().toLong() , ChunkVersion( 0, 0, OID() ).toLong() );
            ASSERT_EQUALS( s.getNumChunks() , 0u );
        }
    };

    class LastChunkTests {
    public:
        void run() {
            BSONObj collection = BSON(CollectionType::ns("test.foo") <<
                                      CollectionType::dropped(false) <<
                                      CollectionType::keyPattern(BSON("a" << 1)) <<
                                      CollectionType::unique(false));

            // 1-chunk collection
            // [10->20)
            BSONArray chunks = BSON_ARRAY(BSON(ChunkType::name("test.foo-a_10") <<
                                               ChunkType::ns("test.foo") <<
                                               ChunkType::min(BSON("a" << 10)) <<
                                               ChunkType::max(BSON("a" << 20))));

            ShardChunkManager s( collection , chunks );
            BSONObj min = BSON( "a" << 10 );
            BSONObj max = BSON( "a" << 20 );

            // if we remove the only chunk, the only version accepted is 0
            ChunkVersion nonZero = ChunkVersion( 99, 0, OID() );
            ASSERT_THROWS( s.cloneMinus( min , max , nonZero ) , UserException );
            ShardChunkManagerPtr empty( s.cloneMinus( min , max , ChunkVersion( 0, 0, OID() ) ) );
            ASSERT_EQUALS( empty->getVersion().toLong() , ChunkVersion( 0, 0, OID() ).toLong() );
            ASSERT_EQUALS( empty->getNumChunks() , 0u );
            BSONObj k = BSON( "a" << 15 << "b" << 0 );
            ASSERT( ! empty->keyBelongsToMe( k ) );

            // we can add a chunk to an empty manager
            // version should be provided
            ASSERT_THROWS( empty->clonePlus( min , max , ChunkVersion( 0, 0, OID() ) ) , UserException );
            ShardChunkManagerPtr cloned( empty->clonePlus( min , max , nonZero ) );
            ASSERT_EQUALS( cloned->getVersion().toLong(), nonZero.toLong() );
            ASSERT_EQUALS( cloned->getNumChunks() , 1u );
            ASSERT( cloned->keyBelongsToMe( k ) );
        }
    };

    class ShardChunkManagerSuite : public Suite {
    public:
        ShardChunkManagerSuite() : Suite ( "shard_chunk_manager" ) {}

        void setupTests() {
            add< BasicTests >();
            add< BasicCompoundTests >();
            add< RangeTests >();
            add< GetNextTests >();
            add< DeletedTests >();
            add< ClonePlusTests >();
            add< ClonePlusExceptionTests >();
            add< CloneMinusTests >();
            add< CloneMinusExceptionTests >();
            add< CloneSplitTests >();
            add< CloneSplitExceptionTests >();
            add< EmptyShardTests >();
            add< LastChunkTests >();
        }
    } shardChunkManagerSuite;

}  // anonymous namespace
