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

    class HasFieldTests {
    public:
        void run() {
            // TEST(HasField, SameKey)
            {
                KeyPattern keyPat( BSON( "x" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x" ) );
            }

            // TEST( HasField, DifferentKey )
            {
                KeyPattern keyPat( BSON( "x" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "y" ) );
            }

            // TEST( HasField, SameKey2Levels )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x.y" ) );
            }

            // TEST( HasField, SameKeyPartial )
            {
                KeyPattern keyPat( BSON( "xyz.a" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "xyz" ) );
            }

            // TEST( HasField, DifferentChildKey )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "x.b" ) );
            }

            // TEST( HasField, DifferentRootKeyDotted )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "a.y" ) );
            }

            // TEST( HasField, SameRootKeyPartial )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x" ) );
            }

            // TEST( HasField, DifferentRootKeyPartial )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "a" ) );
            }

            // TEST( HasField, DifferentMatchingChildKey )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "y" ) );
            }

            // TEST( HasField, SameKey3Level )
            {
                KeyPattern keyPat( BSON( "x.y.z" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x.y.z" ) );
            }

            // TEST( HasField, Same3LevelKeyPartialUpto1 )
            {
                KeyPattern keyPat( BSON( "x.y.z" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x" ) );
            }

            // TEST( HasField, Same3LevelKeyPartialUpto2 )
            {
                KeyPattern keyPat( BSON( "x.y.z" << 1 ) );
                ASSERT_TRUE( keyPat.hasField( "x.y" ) );
            }

            // TEST( HasField, DifferentDottedRoot3Level )
            {
                KeyPattern keyPat( BSON( "x.y.z" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "x.b" ) );
            }

            // TEST( HasField, DifferentRoot3Levels )
            {
                KeyPattern keyPat( BSON( "x.y.z" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "a" ) );
            }

            // TEST( HasField, SameCompoundHasField )
            {
                KeyPattern keyPat( BSON( "x" << 1 << "y" << -1 ) );
                ASSERT_TRUE( keyPat.hasField( "y" ) );
            }

            // TEST( HasField, SameDottedCompoundHasField )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 << "a.b" << -1 ) );
                ASSERT_TRUE( keyPat.hasField( "a.b" ) );
            }

            // TEST( HasField, Same3LevelEmbeddedCompoundHasField )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 << "a.b.c" << -1 ) );
                ASSERT_TRUE( keyPat.hasField( "a" ) );
            }

            // TEST( HasField, DifferentCompoundHasField )
            {
                KeyPattern keyPat( BSON( "x" << 1 << "y" << -1 ) );
                ASSERT_FALSE( keyPat.hasField( "z" ) );
            }

            // TEST( HasField, DifferentDottedCompoundHasField )
            {
                KeyPattern keyPat( BSON( "x.y" << 1 << "a.b" << -1 ) );
                ASSERT_FALSE( keyPat.hasField( "a.j" ) );
            }

            // TEST( HasField, SameRootLongerObjKey )
            {
                KeyPattern keyPat( BSON( "x" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "x.y.z" ) );
            }

            // TEST( HasField, DifferentRootLongerObjKey )
            {
                KeyPattern keyPat( BSON( "x" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "a.b.c" ) );
            }

            // TEST( HasField, DifferentRootPrefixObjKey )
            {
                KeyPattern keyPat( BSON( "xyz.a" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "xy" ) );
            }

            // TEST( HasField, DifferentRootPrefixHasField )
            {
                KeyPattern keyPat( BSON( "xyz" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "xyzabc" ) );
            }

            // TEST( HasField, DifferentRootPartialPrefixObjKey )
            {
                KeyPattern keyPat( BSON( "xyz" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "xy.z" ) );
            }

            // TEST( HasField, DifferentRootPartialPrefixHasField )
            {
                KeyPattern keyPat( BSON( "xy.z" << 1 ) );
                ASSERT_FALSE( keyPat.hasField( "xyz" ) );
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "keypattern" ) {
        }

        void setupTests() {
            add< ExtendRangeBoundTests >();
            add< HasFieldTests >();
        }
    } myall;

} // namespace KeyPatternTests
