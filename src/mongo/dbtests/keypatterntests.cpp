// keypatterntests.cpp - Tests for the KeyPattern class
//

/**
 *    Copyright (C) 2012 10gen Inc.
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


#include "mongo/db/keypattern.h"
#include "mongo/dbtests/dbtests.h"

namespace KeyPatternTests {

    class ExtendRangeBoundTests {
    public:
        void run() {

            BSONObj bound = BSON( "a" << 55 );
            BSONObj longBound = BSON("a" << 55 << "b" << 66);

            //test keyPattern shorter than bound, should fail
            {
                KeyPattern keyPat( BSON( "a" << 1 ) );
                ASSERT_THROWS( keyPat.extendRangeBound( longBound, false ), MsgAssertionException );
            }

            //test keyPattern doesn't match bound, should fail
            {
                KeyPattern keyPat( BSON( "b" << 1 ) );
                ASSERT_THROWS( keyPat.extendRangeBound( bound, false ), MsgAssertionException );
            }
            {
                KeyPattern keyPat( BSON( "a" << 1 << "c" << 1) );
                ASSERT_THROWS( keyPat.extendRangeBound( longBound, false ), MsgAssertionException );
            }

            //test keyPattern same as bound
            {
                KeyPattern keyPat( BSON( "a" << 1 ) );
                BSONObj newB = keyPat.extendRangeBound( bound, false );
                ASSERT_EQUALS( newB , BSON("a" << 55) );
            }
            {
                KeyPattern keyPat( BSON( "a" << 1 ) );
                BSONObj newB = keyPat.extendRangeBound( bound, false );
                ASSERT_EQUALS( newB , BSON("a" << 55) );
            }

            //test keyPattern longer than bound, simple
            {
                KeyPattern keyPat( BSON( "a" << 1 << "b" << 1) );
                BSONObj newB = keyPat.extendRangeBound( bound, false );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MINKEY ) );
            }
            {
                KeyPattern keyPat( BSON( "a" << 1 << "b" << 1) );
                BSONObj newB = keyPat.extendRangeBound( bound, true );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MAXKEY ) );
            }

            //test keyPattern longer than bound, more complex pattern directions
            {
                KeyPattern keyPat( BSON( "a" << 1 << "b" << -1) );
                BSONObj newB = keyPat.extendRangeBound( bound, false );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MAXKEY ) );
            }
            {
                KeyPattern keyPat( BSON( "a" << 1 << "b" << -1) );
                BSONObj newB = keyPat.extendRangeBound( bound, true );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MINKEY ) );
            }
            {

                KeyPattern keyPat( BSON( "a" << 1 << "b" << -1 << "c" << 1 ) );
                BSONObj newB = keyPat.extendRangeBound( bound, false );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MAXKEY << "c" << MINKEY ) );
            }
            {
                KeyPattern keyPat( BSON( "a" << 1 << "b" << -1 << "c" << 1 ) );
                BSONObj newB = keyPat.extendRangeBound( bound, true );
                ASSERT_EQUALS( newB , BSON("a" << 55 << "b" << MINKEY << "c" << MAXKEY ) );
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "keypattern" ) {
        }

        void setupTests() {
            add< ExtendRangeBoundTests >();
        }
    } myall;

} // namespace JsobjTests

