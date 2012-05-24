// jsobjhashingtests.cpp - Tests for hasher.{h,cpp} code
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


#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

namespace JsobjHashingTests {

    class BSONElementHashingTest {
    public:
        void run() {
            int seed = 0;

            //test different oids hash to different things
            long long int oidHash = BSONElementHasher::hash64(
                    BSONObjBuilder().genOID().obj().firstElement() , seed );
            long long int oidHash2 = BSONElementHasher::hash64(
                    BSONObjBuilder().genOID().obj().firstElement() , seed );
            long long int oidHash3 = BSONElementHasher::hash64(
                    BSONObjBuilder().genOID().obj().firstElement() , seed );

            ASSERT_NOT_EQUALS( oidHash , oidHash2 );
            ASSERT_NOT_EQUALS( oidHash , oidHash3 );
            ASSERT_NOT_EQUALS( oidHash3 , oidHash2 );

            //test 32-bit ints, 64-bit ints, doubles hash to same thing
            int i = 3;
            BSONObj p1 = BSON("a" << i);
            long long int intHash = BSONElementHasher::hash64( p1.firstElement() , seed );

            long long int ilong = 3;
            BSONObj p2 = BSON("a" << ilong);
            long long int longHash = BSONElementHasher::hash64( p2.firstElement() , seed );

            double d = 3.1;
            BSONObj p3 = BSON("a" << d);
            long long int doubleHash = BSONElementHasher::hash64( p3.firstElement() , seed );

            ASSERT_EQUALS( intHash, longHash );
            ASSERT_EQUALS( doubleHash, longHash );

            //test different ints don't hash to same thing
            BSONObj p4 = BSON("a" << 4);
            long long int intHash4 = BSONElementHasher::hash64( p4.firstElement() , seed );
            ASSERT_NOT_EQUALS( intHash , intHash4 );

            //test seed makes a difference
            long long int intHash4Seed = BSONElementHasher::hash64( p4.firstElement() , 1 );
            ASSERT_NOT_EQUALS( intHash4 , intHash4Seed );

            //test strings hash to different things
            BSONObj p5 = BSON("a" << "3");
            long long int stringHash = BSONElementHasher::hash64( p5.firstElement() , seed );
            ASSERT_NOT_EQUALS( intHash , stringHash );

            //test regexps and strings hash to different things
            BSONObjBuilder b;
            b.appendRegex("a","3");
            long long int regexHash = BSONElementHasher::hash64( b.obj().firstElement() , seed );
            ASSERT_NOT_EQUALS( stringHash , regexHash );

            //test arrays and subobject hash to different things
            BSONObj p6 = fromjson("{a : {'0' : 0 , '1' : 1}}");
            BSONObj p7 = fromjson("{a : [0,1]}");
            ASSERT_NOT_EQUALS(
                    BSONElementHasher::hash64( p6.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p7.firstElement() , seed )
            );

            //testing sub-document grouping
            BSONObj p8 = fromjson("{x : {a : {}, b : 1}}");
            BSONObj p9 = fromjson("{x : {a : {b : 1}}}");
            ASSERT_NOT_EQUALS(
                    BSONElementHasher::hash64( p8.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p9.firstElement() , seed )
            );

            //testing codeWscope scope squashing
            BSONObjBuilder b1;
            b1.appendCodeWScope("a","print('this is some stupid code')", BSON("a" << 3));
            BSONObj p10 = b1.obj();

            BSONObjBuilder b2;
            b2.appendCodeWScope("a","print('this is some stupid code')", BSON("a" << 3.1));

            BSONObjBuilder b3;
            b3.appendCodeWScope("a","print('this is \nsome stupider code')", BSON("a" << 3));
            ASSERT_EQUALS(
                    BSONElementHasher::hash64( p10.firstElement() , seed ) ,
                    BSONElementHasher::hash64( b2.obj().firstElement() , seed )
            );
            ASSERT_NOT_EQUALS(
                    BSONElementHasher::hash64( p10.firstElement() , seed ) ,
                    BSONElementHasher::hash64( b3.obj().firstElement() , seed )
            );

            //test some recursive squashing
            BSONObj p11 = fromjson("{x : {a : 3 , b : [ 3.1, {c : 3}]}}");
            BSONObj p12 = fromjson("{x : {a : 3.1 , b : [3, {c : 3.0}]}}");
            ASSERT_EQUALS(
                    BSONElementHasher::hash64( p11.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p12.firstElement() , seed )
            );

            //test minkey and maxkey don't hash to same thing
            BSONObj p13 = BSON("a" << MAXKEY);
            BSONObj p14 = BSON("a" << MINKEY);
            ASSERT_NOT_EQUALS(
                    BSONElementHasher::hash64( p13.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p14.firstElement() , seed )
            );

            //test squashing very large doubles and very small doubles
            long long maxInt = std::numeric_limits<long long>::max();
            double smallerDouble = maxInt/2;
            double biggerDouble = ( (double)maxInt )*( (double)maxInt );
            BSONObj p15 = BSON("a" << maxInt );
            BSONObj p16 = BSON("a" << smallerDouble );
            BSONObj p17 = BSON("a" << biggerDouble );
            ASSERT_NOT_EQUALS(
                    BSONElementHasher::hash64( p15.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p16.firstElement() , seed )
            );
            ASSERT_EQUALS(
                    BSONElementHasher::hash64( p15.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p17.firstElement() , seed )
            );

            long long minInt = std::numeric_limits<long long>::min();
            double negativeDouble = -( (double)maxInt )*( (double)maxInt );
            BSONObj p18 = BSON("a" << minInt );
            BSONObj p19 = BSON("a" << negativeDouble );
            ASSERT_EQUALS(
                    BSONElementHasher::hash64( p18.firstElement() , seed ) ,
                    BSONElementHasher::hash64( p19.firstElement() , seed )
            );

        }
    };

    class All : public Suite {
    public:
        All() : Suite( "jsobjhashing" ) {
        }

        void setupTests() {
            add< BSONElementHashingTest >();
        }
    } myall;

} // namespace JsobjTests

