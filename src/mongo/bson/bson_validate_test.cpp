/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/platform/random.h"
#include "mongo/bson/bson_validate.h"

namespace {

    using namespace mongo;

    TEST(BSONValidate, Basic) {
        BSONObj x;
        ASSERT_TRUE( x.valid() );

        x = BSON( "x" << 1 );
        ASSERT_TRUE( x.valid() );
    }

    TEST(BSONValidate, RandomData) {
        PseudoRandom r(17);

        int numValid = 0;
        int numToRun = 1000;
        long long jsonSize = 0;

        for ( int i=0; i<numToRun; i++ ) {
            int size = 1234;

            char* x = new char[size];
            int* xx = reinterpret_cast<int*>(x);
            xx[0] = size;

            for ( int i=4; i<size; i++ ) {
                x[i] = r.nextInt32( 255 );
            }

            x[size-1] = 0;

            BSONObj o( x );

            ASSERT_EQUALS( size, o.objsize() );

            if ( o.valid() ) {
                numValid++;
                jsonSize += o.jsonString().size();
                ASSERT_OK( validateBSON( o.objdata(), o.objsize() ) );
            }
            else {
                ASSERT_NOT_OK( validateBSON( o.objdata(), o.objsize() ) );
            }

            delete[] x;
        }

        log() << "RandomData: didn't crash valid/total: " << numValid << "/" << numToRun
              << " (want few valid ones)"
              << " jsonSize: " << jsonSize << endl;
    }

    TEST(BSONValidate, MuckingData1) {

        BSONObj theObject;

        {
            BSONObjBuilder b;
            b.append( "name" , "eliot was here" );
            b.append( "yippee" , "asd" );
            BSONArrayBuilder a( b.subarrayStart( "arr" ) );
            for ( int i=0; i<100; i++ ) {
                a.append( BSON( "x" << i << "who" << "me" << "asd" << "asd" ) );
            }
            a.done();
            b.done();

            theObject = b.obj();
        }

        int numValid = 0;
        int numToRun = 1000;
        long long jsonSize = 0;

        for ( int i=4; i<theObject.objsize()-1; i++ ) {
            BSONObj mine = theObject.copy();

            char* data = const_cast<char*>(mine.objdata());

            data[ i ] = 200;

            numToRun++;
            if ( mine.valid() ) {
                numValid++;
                jsonSize += mine.jsonString().size();
                ASSERT_OK( validateBSON( mine.objdata(), mine.objsize() ) );
            }
            else {
                ASSERT_NOT_OK( validateBSON( mine.objdata(), mine.objsize() ) );
            }

        }

        log() << "MuckingData1: didn't crash valid/total: " << numValid << "/" << numToRun
              << " (want few valid ones) "
              << " jsonSize: " << jsonSize << endl;
    }

    TEST( BSONValidate, Fuzz ) {
        int64_t seed = time( 0 );
        log() << "BSONValidate Fuzz random seed: " << seed << endl;
        PseudoRandom randomSource( seed );

        BSONObj original = BSON( "one" << 3 <<
                                 "two" << 5 <<
                                 "three" << BSONObj() <<
                                 "four" << BSON( "five" << BSON( "six" << 11 ) ) <<
                                 "seven" << BSON_ARRAY( "a" << "bb" << "ccc" << 5 ) <<
                                 "eight" << BSONDBRef( "rrr", OID( "01234567890123456789aaaa" ) ) <<
                                 "_id" << OID( "deadbeefdeadbeefdeadbeef" ) <<
                                 "nine" << BSONBinData( "\x69\xb7", 2, BinDataGeneral ) <<
                                 "ten" << Date_t( 44 ) <<
                                 "eleven" << BSONRegEx( "foooooo", "i" ) );
        
        int32_t fuzzFrequencies[] = { 2, 10, 20, 100, 1000 };
        for( size_t i = 0; i < sizeof( fuzzFrequencies ) / sizeof( int32_t ); ++i ) {
            int32_t fuzzFrequency = fuzzFrequencies[ i ];

            // Copy the 'original' BSONObj to 'buffer'.
            scoped_array<char> buffer( new char[ original.objsize() ] );
            memcpy( buffer.get(), original.objdata(), original.objsize() );

            // Randomly flip bits in 'buffer', with probability determined by 'fuzzFrequency'. The
            // first four bytes, representing the size of the object, are excluded from bit
            // flipping.
            for( int32_t byteIdx = 4; byteIdx < original.objsize(); ++byteIdx ) {
                for( int32_t bitIdx = 0; bitIdx < 8; ++bitIdx ) {
                    if ( randomSource.nextInt32( fuzzFrequency ) == 0 ) {
                        reinterpret_cast<unsigned char&>( buffer[ byteIdx ] ) ^= ( 1U << bitIdx );
                    }
                }
            }
            BSONObj fuzzed( buffer.get() );

            // Check that the two validation implementations agree (and neither crashes).
            ASSERT_EQUALS( fuzzed.valid(),
                           validateBSON( fuzzed.objdata(), fuzzed.objsize() ).isOK() );
        }
    }

    TEST( BSONValidateFast, Empty ) {
        BSONObj x;
        ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );
    }

    TEST( BSONValidateFast, RegEx ) {
        BSONObjBuilder b;
        b.appendRegex( "foo", "i" );
        BSONObj x = b.obj();
        ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );
    }

    TEST(BSONValidateFast, Simple0 ) {
        BSONObj x;
        ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );

        x = BSON( "foo" << 17 << "bar" << "eliot" );
        ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );

    }

    TEST(BSONValidateFast, Simple2 ) {
        char buf[64];
        for ( int i=1; i<=JSTypeMax; i++ ) {
            BSONObjBuilder b;
            sprintf( buf, "foo%d", i );
            b.appendMinForType( buf, i );
            sprintf( buf, "bar%d", i );
            b.appendMaxForType( buf, i );
            BSONObj x = b.obj();
            ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );
        }
    }


    TEST(BSONValidateFast, Simple3 ) {
        BSONObjBuilder b;
        char buf[64];
        for ( int i=1; i<=JSTypeMax; i++ ) {
            sprintf( buf, "foo%d", i );
            b.appendMinForType( buf, i );
            sprintf( buf, "bar%d", i );
            b.appendMaxForType( buf, i );
        }
        BSONObj x = b.obj();
        ASSERT_OK( validateBSON( x.objdata(), x.objsize() ) );
    }

    TEST(BSONValidateFast, NestedObject) {
        BSONObj x = BSON( "a" << 1 << "b" << BSON("c" << 2 << "d" << BSONArrayBuilder().obj() << "e" << BSON_ARRAY("1" << 2 << 3)));
        ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
        ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize() / 2));
    }

}
