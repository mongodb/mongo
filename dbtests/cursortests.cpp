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

#include "dbtests.h"

namespace CursorTests {
    
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
    
    class All : public Suite {
    public:
        All() {
            add< IdSetTests::BasicSize >();
            add< IdSetTests::Upgrade >();
        }
    };
} // namespace CursorTests

UnitTest::TestPtr cursorTests() {
    return UnitTest::createSuite< CursorTests::All >();
}
