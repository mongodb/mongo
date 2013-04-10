// index_set_tests.cpp

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

#include "mongo/unittest/unittest.h"

#include "mongo/db/index_set.h"

namespace mongo {

    TEST( IndexPathSetTest, Simple1 ) {
        IndexPathSet a;
        a.addPath( "a.b" );
        ASSERT_TRUE( a.mightBeIndexed( "a.b" ) );
        ASSERT_TRUE( a.mightBeIndexed( "a" ) );
        ASSERT_TRUE( a.mightBeIndexed( "a.b.c" ) );
        ASSERT_TRUE( a.mightBeIndexed( "a.$.b" ) );

        ASSERT_FALSE( a.mightBeIndexed( "b" ) );
        ASSERT_FALSE( a.mightBeIndexed( "a.c" ) );
    }

    TEST( IndexPathSetTest, Simple2 ) {
        IndexPathSet a;
        a.addPath( "ab" );
        ASSERT_FALSE( a.mightBeIndexed( "a" ) );
    }


    TEST( IndexPathSetTest, getCanonicalIndexField1 ) {
        string x;

        ASSERT_FALSE( getCanonicalIndexField( "a", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "aaa", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "a.b", &x ) );

        ASSERT_TRUE( getCanonicalIndexField( "a.$", &x ) );
        ASSERT_EQUALS( x, "a" );
        ASSERT_TRUE( getCanonicalIndexField( "a.0", &x ) );
        ASSERT_EQUALS( x, "a" );
        ASSERT_TRUE( getCanonicalIndexField( "a.123", &x ) );
        ASSERT_EQUALS( x, "a" );

        ASSERT_TRUE( getCanonicalIndexField( "a.$.b", &x ) );
        ASSERT_EQUALS( x, "a.b" );
        ASSERT_TRUE( getCanonicalIndexField( "a.0.b", &x ) );
        ASSERT_EQUALS( x, "a.b" );
        ASSERT_TRUE( getCanonicalIndexField( "a.123.b", &x ) );
        ASSERT_EQUALS( x, "a.b" );

        ASSERT_FALSE( getCanonicalIndexField( "a.$ref", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "a.$ref.b", &x ) );


        ASSERT_FALSE( getCanonicalIndexField( "a.c$d.b", &x ) );

        ASSERT_FALSE( getCanonicalIndexField( "a.123a", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "a.a123", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "a.123a.b", &x ) );
        ASSERT_FALSE( getCanonicalIndexField( "a.a123.b", &x ) );

        ASSERT_FALSE( getCanonicalIndexField( "a.", &x ) );
    }

}
