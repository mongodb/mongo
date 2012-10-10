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

namespace {
    
    using namespace mongo;

    TEST(BSONValidate, Basic) {
        BSONObj x;
        ASSERT_TRUE( x.valid() );
        
        x = BSON( "x" << 1 );
        ASSERT_TRUE( x.valid() );
    }

#ifndef _WIN32 // this is temporary till I commit a new Random class
    
    TEST(BSONValidate, RandomData) {
        
        unsigned seed = 17;

        int numValid = 0;
        int numToRun = 1000;
        long long jsonSize = 0;

        for ( int i=0; i<numToRun; i++ ) {
            int size = 1234;
            
            char* x = new char[size];
            int* xx = reinterpret_cast<int*>(x);
            xx[0] = size;
            
            for ( int i=4; i<size; i++ ) {
                x[i] = rand_r(&seed) % 255;
            }
            
            x[size-1] = 0;

            BSONObj o( x );

            ASSERT_EQUALS( size, o.objsize() );

            if ( o.valid() ) {
                numValid++;
                jsonSize += o.jsonString().size();
            }

            delete[] x;
        }

        log() << "RandomData: didn't crash valid/total: " << numValid << "/" << numToRun << " (want few valid ones)" 
              << " jsonSize: " << jsonSize << endl;
    }
#endif

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
            }
   
        }


        log() << "MuckingData1: didn't crash valid/total: " << numValid << "/" << numToRun << " (want few valid ones) " 
              << " jsonSize: " << jsonSize << endl;        
    }

}
