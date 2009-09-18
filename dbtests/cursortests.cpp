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

#include "../db/clientcursor.h"
#include "../db/instance.h"
#include "../db/btree.h"

#include "dbtests.h"

namespace CursorTests {
    
    typedef IdSet_Deprecated IdSet;

    namespace IdSetTests {
        
        class BasicSize {
        public:
            void run() {
                IdSet a;
                IdSet b;
                int baseSize = BSON( "a" << 4 ).objsize() + sizeof( BSONObj );
                ASSERT_EQUALS( 0, IdSet::aggregateSize() );
                a.put( BSON( "a" << 4 ) );
                ASSERT_EQUALS( baseSize, a.mySize() );
                a.put( BSON( "ab" << 4 ) );
                ASSERT_EQUALS( baseSize * 2 + 1, a.mySize() );
                ASSERT_EQUALS( baseSize * 2 + 1, IdSet::aggregateSize() );
                b.put( BSON( "abc" << 4 ) );
                ASSERT_EQUALS( baseSize + 2, b.mySize() );
                ASSERT_EQUALS( baseSize * 3 + 1 + 2, IdSet::aggregateSize() );                
            }
            ~BasicSize() {
                if ( 0 != IdSet::aggregateSize() )
                    FAIL( "aggregateSize not reset" );
            }
        private:
            dblock lk_;
        };
        
        class Upgrade {
        public:
            Upgrade() : num_() {}
            void run() {
                setClient( "unittests.bar" );
                
                IdSet::maxSize_ = ( BSON( "_id" << int( 1 ) ).objsize() + sizeof( BSONObj ) - 1 ) * 8;
                
                IdSet a;
                IdSet b;
                ASSERT( a.inMem() );
                ASSERT( b.inMem() );
                a.put( obj() );
                b.put( obj() );
                b.put( obj() );
                b.put( obj() );
                b.mayUpgradeStorage( "b" );
                ASSERT( b.inMem() );
                a.put( obj() );
                a.put( obj() );
                a.mayUpgradeStorage( "a" );
                ASSERT( a.inMem() );
                a.put( obj() );
                a.mayUpgradeStorage( "a" );
                ASSERT( !a.inMem() );
                b.put( obj() );
                b.mayUpgradeStorage( "b" );
                ASSERT( !b.inMem() );
                
                ASSERT( a.get( obj( 0 ) ) );
                for( int i = 1; i < 4; ++i )
                    ASSERT( b.get( obj( i ) ) );
                for( int i = 4; i < 7; ++i )
                    ASSERT( a.get( obj( i ) ) );
                ASSERT( b.get( obj( 7 ) ) );
            }
            ~Upgrade() {
                setClient( "local.temp" );
                if ( nsdetails( "local.temp.clientcursor.a" ) || nsdetails( "local.temp.clientcursor.b" ) )
                    FAIL( "client cursor temp collection not deleted" );
            }
        private:
            BSONObj obj( int i = -1 ) {
                if ( i == -1 )
                    i = num_++;
                return BSON( "_id" << i );
            }
            dblock lk_;
            int num_;
        };

    } // namespace IdSetTests

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
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->indexes[ 1 ], b, 1 );
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
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->indexes[ 1 ], b, 1 );
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
                BtreeCursor c( nsdetails( ns ), 1, nsdetails( ns )->indexes[ 1 ], b, -1 );
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
     
    } // namespace MultiBtreeCursorTests
    
    class All : public Suite {
    public:
        All() : Suite( "cursor" ){}
        
        void setupTests(){
            add< IdSetTests::BasicSize >();
            add< IdSetTests::Upgrade >();
            add< BtreeCursorTests::MultiRange >();
            add< BtreeCursorTests::MultiRangeGap >();
            add< BtreeCursorTests::MultiRangeReverse >();
        }
    } myall;
} // namespace CursorTests
