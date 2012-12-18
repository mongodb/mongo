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
                ASSERT( validateBSON( o.objdata(), o.objsize() ).isOK() );
            }
            else {
                ASSERT( !validateBSON( o.objdata(), o.objsize() ).isOK() );
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
                ASSERT( validateBSON( mine.objdata(), mine.objsize() ).isOK() );
            }
            else {
                ASSERT( !validateBSON( mine.objdata(), mine.objsize() ).isOK() );
            }

        }

        log() << "MuckingData1: didn't crash valid/total: " << numValid << "/" << numToRun
              << " (want few valid ones) "
              << " jsonSize: " << jsonSize << endl;
    }

    TEST( BSONValidateFast, Empty ) {
        BSONObj x;
        ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );
    }

    TEST( BSONValidateFast, RegEx ) {
        BSONObjBuilder b;
        b.appendRegex( "foo", "i" );
        BSONObj x = b.obj();
        ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );
    }

    TEST(BSONValidateFast, Simple0 ) {
        BSONObj x;
        ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );

        x = BSON( "foo" << 17 << "bar" << "eliot" );
        ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );

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
            ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );
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
        ASSERT( validateBSON( x.objdata(), x.objsize() ).isOK() );
    }


}
