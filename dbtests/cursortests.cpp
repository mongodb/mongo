// cusrortests.cpp // cursor related unit tests
//

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

#include "stdafx.h"
#include "../db/db.h"
#include "../db/clientcursor.h"
#include "../db/instance.h"
#include "../db/btree.h"
#include "dbtests.h"

namespace CursorTests {
    
    namespace BtreeCursorTests {

        class MultiRange {
        public:
            void run() {
                dblock lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRange";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                BoundList b;
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 1 ), BSON( "" << 2 ) ) );
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 4 ), BSON( "" << 6 ) ) );
                setClient( ns );
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->idx(1), b, 1 );
                ASSERT_EQUALS( "BtreeCursor a_1 multi", c.toString() );
                double expected[] = { 1, 2, 4, 5, 6 };
                for( int i = 0; i < 5; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };

        class MultiRangeGap {
        public:
            void run() {
                dblock lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRangeGap";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    for( int i = 100; i < 110; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                BoundList b;
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << -50 ), BSON( "" << 2 ) ) );
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 40 ), BSON( "" << 60 ) ) );
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 109 ), BSON( "" << 200 ) ) );
                setClient( ns );
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->idx(1), b, 1 );
                ASSERT_EQUALS( "BtreeCursor a_1 multi", c.toString() );
                double expected[] = { 0, 1, 2, 109 };
                for( int i = 0; i < 4; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };
     
        class MultiRangeReverse {
        public:
            void run() {
                dblock lk;
                const char *ns = "unittests.cursortests.BtreeCursorTests.MultiRangeReverse";
                {
                    DBDirectClient c;
                    for( int i = 0; i < 10; ++i )
                        c.insert( ns, BSON( "a" << i ) );
                    ASSERT( c.ensureIndex( ns, BSON( "a" << 1 ) ) );
                }
                BoundList b;
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 6 ), BSON( "" << 4 ) ) );
                b.push_back( pair< BSONObj, BSONObj >( BSON( "" << 2 ), BSON( "" << 1 ) ) );
                setClient( ns );
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->idx(1), b, -1 );
                ASSERT_EQUALS( "BtreeCursor a_1 reverse multi", c.toString() );
                double expected[] = { 6, 5, 4, 2, 1 };
                for( int i = 0; i < 5; ++i ) {
                    ASSERT( c.ok() );
                    ASSERT_EQUALS( expected[ i ], c.currKey().firstElement().number() );
                    c.advance();
                }
                ASSERT( !c.ok() );
            }
        };
     
    } // namespace BtreeCursorTests
    
    class All : public Suite {
    public:
        All() : Suite( "cursor" ){}
        
        void setupTests(){
            add< BtreeCursorTests::MultiRange >();
            add< BtreeCursorTests::MultiRangeGap >();
            add< BtreeCursorTests::MultiRangeReverse >();
        }
    } myall;
} // namespace CursorTests
