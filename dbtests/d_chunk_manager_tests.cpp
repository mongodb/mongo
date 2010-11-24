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
#include "dbtests.h"

#include "../s/d_chunk_manager.h"

namespace {

    class BasicTests {
    public:
        void run() {
            BSONObj collection = BSON( "_id" << "test.foo" << "key" << BSON( "a" << 1 ) << "unique" << false );
            BSONArray chunks   = BSON_ARRAY( BSON("_id" << "test.foo-a_MinKey" << "ns" << "test.foo" << 
                                                  "min" << BSON( "a" << MINKEY ) << "max" << BSON( "a" << MAXKEY ) ) );

            ShardChunkManager s ( collection , chunks );
            
            BSONObj k1 = BSON( "a" << 1 << "b" << 2 );
            ASSERT( s.belongsToMe( k1 ) );
        }
    };

    class ShardChunkManagerSuite : public Suite {
    public:
        ShardChunkManagerSuite() : Suite ( "shard_chunk_manager" ) {}

        void setupTests() {
            add< BasicTests >();
        }
    } shardChunkManagerSuite;

}  // anonymous namespace
