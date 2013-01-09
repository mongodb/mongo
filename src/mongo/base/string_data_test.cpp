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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::StringData;
    using std::string;

    TEST(Construction, Empty) {
        StringData strData;
        ASSERT_EQUALS(strData.size(), 0U);
        ASSERT_TRUE(strData.rawData() == NULL);
    }

    TEST(Construction, FromStdString) {
        std::string base("aaa");
        StringData strData(base);
        ASSERT_EQUALS(strData.size(), base.size());
        ASSERT_EQUALS(strData.toString(), base);
    }

    TEST(Construction, FromCString) {
        std::string base("aaa");
        StringData strData(base.c_str());
        ASSERT_EQUALS(strData.size(), base.size());
        ASSERT_EQUALS(strData.toString(), base);
    }

    TEST(Construction, FromNullCString) {
        char* c = NULL;
        StringData strData(c);
        ASSERT_EQUALS(strData.size(), 0U);
        ASSERT_TRUE(strData.rawData() == NULL);
    }

    TEST(Construction, FromLiteral) {
        StringData strData("ccc", StringData::LiteralTag());
        ASSERT_EQUALS(strData.size(), 3U);
        ASSERT_EQUALS(strData.toString(), string("ccc"));
    }

    TEST(Comparison, BothEmpty) {
        StringData empty("");
        ASSERT_TRUE(empty == empty);
        ASSERT_FALSE(empty != empty);
        ASSERT_FALSE(empty > empty);
        ASSERT_TRUE(empty >= empty);
        ASSERT_FALSE(empty < empty);
        ASSERT_TRUE(empty <= empty);
    }

    TEST(Comparison, BothNonEmptyOnSize) {
        StringData a("a");
        StringData aa("aa");
        ASSERT_FALSE(a == aa);
        ASSERT_TRUE(a != aa);
        ASSERT_FALSE(a > aa);
        ASSERT_FALSE(a >= aa);
        ASSERT_TRUE(a >= a);
        ASSERT_TRUE(a < aa);
        ASSERT_TRUE(a <= aa);
        ASSERT_TRUE(a <= a);
    }

    TEST(Comparison, BothNonEmptyOnContent) {
        StringData a("a");
        StringData b("b");
        ASSERT_FALSE(a == b);
        ASSERT_TRUE(a != b);
        ASSERT_FALSE(a > b);
        ASSERT_FALSE(a >= b);
        ASSERT_TRUE(a < b);
        ASSERT_TRUE(a <= b);
    }

    TEST(Comparison, MixedEmptyAndNot) {
        StringData empty("");
        StringData a("a");
        ASSERT_FALSE(a == empty);
        ASSERT_TRUE(a != empty);
        ASSERT_TRUE(a > empty);
        ASSERT_TRUE(a >= empty);
        ASSERT_FALSE(a < empty);
        ASSERT_FALSE(a <= empty);
    }

    TEST(Find, Char1) {
        ASSERT_EQUALS( string::npos, StringData( "foo" ).find( 'a' ) );
        ASSERT_EQUALS( 0U, StringData( "foo" ).find( 'f' ) );
        ASSERT_EQUALS( 1U, StringData( "foo" ).find( 'o' ) );
    }

    TEST(Find, Str1 ) {
        ASSERT_EQUALS( string::npos, StringData( "foo" ).find( "asdsadasda" ) );
        ASSERT_EQUALS( string::npos, StringData( "foo" ).find( "a" ) );
        ASSERT_EQUALS( string::npos, StringData( "foo" ).find( "food" ) );
        ASSERT_EQUALS( string::npos, StringData( "foo" ).find( "ooo" ) );

        ASSERT_EQUALS( 0U, StringData( "foo" ).find( "f" ) );
        ASSERT_EQUALS( 0U, StringData( "foo" ).find( "fo" ) );
        ASSERT_EQUALS( 0U, StringData( "foo" ).find( "foo" ) );
        ASSERT_EQUALS( 1U, StringData( "foo" ).find( "o" ) );
        ASSERT_EQUALS( 1U, StringData( "foo" ).find( "oo" ) );

        ASSERT_EQUALS( string("foo").find( "" ), StringData("foo").find( "" ) );
    }

    // this is to verify we match std::string
#define SUBSTR_TEST_HELP(big,small,start,len)   \
    ASSERT_EQUALS( (string)small, ((string)big).substr( start, len ) ); \
    ASSERT_EQUALS( StringData(small), StringData(big).substr( start, len ) );

    TEST(Substr, Simple1 ) {
        SUBSTR_TEST_HELP( "abcde", "abcde", 0, 10 );
        SUBSTR_TEST_HELP( "abcde", "abcde", 0, 5 );
        SUBSTR_TEST_HELP( "abcde", "abc", 0, 3 );
        SUBSTR_TEST_HELP( "abcde", "cde", 2, 5 );
        SUBSTR_TEST_HELP( "abcde", "cde", 2, 3 );
        SUBSTR_TEST_HELP( "abcde", "cd", 2, 2 );
        SUBSTR_TEST_HELP( "abcde", "cd", 2, 2 );
    }

    TEST( equalCaseInsensitiveTest, Simple1 ) {
        ASSERT( StringData( "abc" ).equalCaseInsensitive( "abc" ) );
        ASSERT( StringData( "abc" ).equalCaseInsensitive( "ABC" ) );
        ASSERT( StringData( "ABC" ).equalCaseInsensitive( "abc" ) );
        ASSERT( StringData( "ABC" ).equalCaseInsensitive( "ABC" ) );
        ASSERT( StringData( "ABC" ).equalCaseInsensitive( "AbC" ) );
        ASSERT( !StringData( "ABC" ).equalCaseInsensitive( "AbCd" ) );
        ASSERT( !StringData( "ABC" ).equalCaseInsensitive( "AdC" ) );
    }

    TEST(StartsWith, Simple) {
        ASSERT(StringData("").startsWith(""));
        ASSERT(!StringData("").startsWith("x"));
        ASSERT(StringData("abcde").startsWith(""));
        ASSERT(StringData("abcde").startsWith("a"));
        ASSERT(StringData("abcde").startsWith("ab"));
        ASSERT(StringData("abcde").startsWith("abc"));
        ASSERT(StringData("abcde").startsWith("abcd"));
        ASSERT(StringData("abcde").startsWith("abcde"));
        ASSERT(!StringData("abcde").startsWith("abcdef"));
        ASSERT(!StringData("abcde").startsWith("abdce"));
        ASSERT(StringData("abcde").startsWith(StringData("abcdeXXXX").substr(0, 4)));
        ASSERT(!StringData("abcde").startsWith(StringData("abdef").substr(0, 4)));
        ASSERT(!StringData("abcde").substr(0, 3).startsWith("abcd"));
    }

    TEST(EndsWith, Simple) {
        //ASSERT(StringData("").endsWith(""));
        ASSERT(!StringData("").endsWith("x"));
        //ASSERT(StringData("abcde").endsWith(""));
        ASSERT(StringData("abcde").endsWith(StringData("e", 0)));
        ASSERT(StringData("abcde").endsWith("e"));
        ASSERT(StringData("abcde").endsWith("de"));
        ASSERT(StringData("abcde").endsWith("cde"));
        ASSERT(StringData("abcde").endsWith("bcde"));
        ASSERT(StringData("abcde").endsWith("abcde"));
        ASSERT(!StringData("abcde").endsWith("0abcde"));
        ASSERT(!StringData("abcde").endsWith("abdce"));
        ASSERT(StringData("abcde").endsWith(StringData("bcdef").substr(0, 4)));
        ASSERT(!StringData("abcde").endsWith(StringData("bcde", 3)));
        ASSERT(!StringData("abcde").substr(0, 3).endsWith("cde"));
    }

} // unnamed namespace
