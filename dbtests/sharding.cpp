// sharding.cpp : some unit tests for sharding internals

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "../client/parallel.h"

namespace ShardingTests {

    namespace serverandquerytests {
        class test1 {
        public:
            void run() {
                ServerAndQuery a( "foo:1" , BSON( "a" << GT << 0 << LTE << 100 ) );
                ServerAndQuery b( "foo:1" , BSON( "a" << GT << 200 << LTE << 1000 ) );

                ASSERT( a < b );
                ASSERT( ! ( b < a ) );

                set<ServerAndQuery> s;
                s.insert( a );
                s.insert( b );

                ASSERT_EQUALS( (unsigned int)2 , s.size() );
            }
        };
    }

    class All : public Suite {
    public:
        All() : Suite( "sharding" ) {
        }

        void setupTests() {
            add< serverandquerytests::test1 >();
        }
    } myall;

}
