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
                ASSERT_EQUALS( 0, IdSet::aggregateSize() );
                a.put( BSON( "a" << 4 ) );
                ASSERT_EQUALS( 24, a.mySize() );
                a.put( BSON( "ab" << 4 ) );
                ASSERT_EQUALS( 49, a.mySize() );
                ASSERT_EQUALS( 49, IdSet::aggregateSize() );
                b.put( BSON( "abc" << 4 ) );
                ASSERT_EQUALS( 26, b.mySize() );
                ASSERT_EQUALS( 75, IdSet::aggregateSize() );                
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
                setClient( "foo.bar" );
                
                IdSet::maxSize_ = 200;
                
                IdSet a;
                IdSet b;
                ASSERT( a.inMem() );
                ASSERT( b.inMem() );
                a.put( twentySix() );
                b.put( twentySix() );
                b.put( twentySix() );
                b.put( twentySix() );
                b.mayUpgradeStorage( "b" );
                ASSERT( b.inMem() );
                a.put( twentySix() );
                a.put( twentySix() );
                a.mayUpgradeStorage( "a" );
                ASSERT( a.inMem() );
                a.put( twentySix() );
                a.mayUpgradeStorage( "a" );
                ASSERT( !a.inMem() );
                b.put( twentySix() );
                b.mayUpgradeStorage( "b" );
                ASSERT( !b.inMem() );
                
                ASSERT( a.get( twentySix( 0 ) ) );
                for( int i = 1; i < 4; ++i )
                    ASSERT( b.get( twentySix( i ) ) );
                for( int i = 4; i < 7; ++i )
                    ASSERT( a.get( twentySix( i ) ) );
                ASSERT( b.get( twentySix( 7 ) ) );
            }
            ~Upgrade() {
                setClient( "local.temp" );
                if ( nsdetails( "local.temp.clientcursor.a" ) || nsdetails( "local.temp.clientcursor.b" ) )
                    FAIL( "client cursor temp collection not deleted" );
            }
        private:
            BSONObj twentySix( int i = -1 ) {
                if ( i == -1 )
                    i = num_++;
                return BSON( "_id" << i );
            }
            dblock lk_;
            int num_;
        };

    } // namespace IdSetTests
    
    class All : public UnitTest::Suite {
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