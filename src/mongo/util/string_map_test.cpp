// string_map_test.cpp

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

#include "mongo/platform/random.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/string_map.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace {
    using namespace mongo;

    TEST(StringMapTest, Hash1) {
        StringMapDefaultHash h;
        ASSERT_EQUALS( h(""), h("") );
        ASSERT_EQUALS( h("a"), h("a") );
        ASSERT_EQUALS( h("abc"), h("abc") );

        ASSERT_NOT_EQUALS( h(""), h("a") );
        ASSERT_NOT_EQUALS( h("a"), h("ab") );

        ASSERT_NOT_EQUALS( h("foo28"), h("foo35") );
    }

#define equalsBothWays(a,b) \
    ASSERT_TRUE( e( (a), (b) ) );  \
    ASSERT_TRUE( e( (b), (a) ) );

#define notEqualsBothWays(a,b) \
    ASSERT_FALSE( e( (a), (b) ) );  \
    ASSERT_FALSE( e( (b), (a) ) );

    TEST(StringMapTest, Equals1 ){
        StringMapDefaultEqual e;

        equalsBothWays( "", "" );
        equalsBothWays( "a", "a" );
        equalsBothWays( "bbbbb", "bbbbb" );

        notEqualsBothWays( "", "a" );
        notEqualsBothWays( "a", "b" );
        notEqualsBothWays( "abc", "def" );
        notEqualsBothWays( "abc", "defasdasd" );
    }

    TEST(StringMapTest, Basic1) {
        StringMap<int> m;
        ASSERT_EQUALS( 0U, m.size() );
        m["eliot"] = 5;
        ASSERT_EQUALS( 5, m["eliot"] );
        ASSERT_EQUALS( 1U, m.size() );
    }

    TEST(StringMapTest, Big1) {
        StringMap<int> m;
        char buf[64];

        for ( int i=0; i<10000; i++ ) {
            sprintf( buf, "foo%d", i );
            m[buf] = i;
        }

        for ( int i=0; i<10000; i++ ) {
            sprintf( buf, "foo%d", i );
            ASSERT_EQUALS( m[buf], i );
        }
    }

    TEST(StringMapTest, find1 ) {
        StringMap<int> m;

        ASSERT_TRUE( m.end() == m.find( "foo" ) );

        m["foo"] = 5;
        StringMap<int>::const_iterator i = m.find( "foo" );
        ASSERT_TRUE( i != m.end() );
        ASSERT_EQUALS( 5, i->second );
        ASSERT_EQUALS( "foo", i->first );
        ++i;
        ASSERT_TRUE( i == m.end() );
    }


    TEST(StringMapTest, Erase1 ) {
        StringMap<int> m;
        char buf[64];

        m["eliot"] = 5;
        ASSERT_EQUALS( 5, m["eliot"] );
        m.erase( "eliot" );
        ASSERT( m.end() == m.find( "eliot" ) );
        ASSERT_EQUALS( 0, m["eliot"] );
        m.erase( "eliot" );
        ASSERT( m.end() == m.find( "eliot" ) );

        size_t before = m.capacity();
        for ( int i = 0; i < 10000; i++ ) {
            sprintf( buf, "foo%d", i );
            m[buf] = i;
            ASSERT_EQUALS( i, m[buf] );
            m.erase( buf );
            ASSERT( m.end() == m.find( buf ) );
        }
        ASSERT_EQUALS( before, m.capacity() );
    }

    TEST( StringMapTest, Iterator1 ) {
        StringMap<int> m;
        ASSERT( m.begin() == m.end() );
    }

    TEST( StringMapTest, Iterator2 ) {
        StringMap<int> m;
        m["eliot"] = 5;
        StringMap<int>::const_iterator i = m.begin();
        ASSERT_EQUALS( "eliot", i->first );
        ASSERT_EQUALS( 5, i->second );
        ++i;
        ASSERT( i == m.end() );
    }

    TEST( StringMapTest, Iterator3 ) {
        StringMap<int> m;
        m["eliot"] = 5;
        m["bob"] = 6;
        StringMap<int>::const_iterator i = m.begin();
        int sum = 0;
        for ( ; i != m.end(); ++i ) {
            sum += i->second;
        }
        ASSERT_EQUALS( 11, sum );
    }


    TEST( StringMapTest, Copy1 ) {
        StringMap<int> m;
        m["eliot"] = 5;
        StringMap<int> y = m;
        ASSERT_EQUALS( 5, y["eliot"] );

        m["eliot"] = 6;
        ASSERT_EQUALS( 6, m["eliot"] );
        ASSERT_EQUALS( 5, y["eliot"] );
    }

    TEST( StringMapTest, Assign ) {
        StringMap<int> m;
        m["eliot"] = 5;

        StringMap<int> y;
        y["eliot"] = 6;
        ASSERT_EQUALS( 6, y["eliot"] );

        y = m;
        ASSERT_EQUALS( 5, y["eliot"] );
    }
}
