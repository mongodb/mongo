// namespacestring_test.cpp

/*    Copyright 2012 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/unittest/unittest.h"

#include "mongo/db/namespace_string.h"

namespace mongo {

    TEST( NamespaceStringTest, Normal ) {
        ASSERT( NamespaceString::normal( "a" ) );
        ASSERT( NamespaceString::normal( "a.b" ) );
        ASSERT( NamespaceString::normal( "a.b.c" ) );

        ASSERT( !NamespaceString::normal( "a.b.$c" ) );
        ASSERT( !NamespaceString::normal( "a.b.$.c" ) );

        ASSERT( NamespaceString::normal( "local.oplog.$main" ) );
        ASSERT( NamespaceString::normal( "local.oplog.rs" ) );
    }

    TEST( NamespaceStringTest, Oplog ) {
        ASSERT( !NamespaceString::oplog( "a" ) );
        ASSERT( !NamespaceString::oplog( "a.b" ) );

        ASSERT( NamespaceString::oplog( "local.oplog.rs" ) );
        ASSERT( NamespaceString::oplog( "local.oplog.foo" ) );
        ASSERT( NamespaceString::oplog( "local.oplog.$main" ) );
        ASSERT( NamespaceString::oplog( "local.oplog.$foo" ) );
    }

    TEST( NamespaceStringTest, Special ) {
        ASSERT( NamespaceString::special( "a.$.b" ) );
        ASSERT( NamespaceString::special( "a.system.foo" ) );
        ASSERT( !NamespaceString::special( "a.foo" ) );
        ASSERT( !NamespaceString::special( "a.foo.system.bar" ) );
        ASSERT( !NamespaceString::special( "a.systemfoo" ) );
    }

    TEST( NamespaceStringTest, DatabaseValidNames ) {
        ASSERT( NamespaceString::validDBName( "foo" ) );
        ASSERT( !NamespaceString::validDBName( "foo/bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo.bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo.bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo\\bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo\"bar" ) );
        ASSERT( !NamespaceString::validDBName( StringData( "a\0b", StringData::LiteralTag() ) ) );
#ifdef _WIN32
        ASSERT( !NamespaceString::validDBName( "foo*bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo<bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo>bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo:bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo|bar" ) );
        ASSERT( !NamespaceString::validDBName( "foo?bar" ) );
#endif

        ASSERT( NamespaceString::normal( "asdads" ) );
        ASSERT( !NamespaceString::normal( "asda$ds" ) );
        ASSERT( NamespaceString::normal( "local.oplog.$main" ) );
    }

    TEST( NamespaceStringTest, CollectionComponentValidNames ) {
        ASSERT( NamespaceString::validCollectionComponent( "a.b" ) );
        ASSERT( NamespaceString::validCollectionComponent( "a.b" ) );
        ASSERT( !NamespaceString::validCollectionComponent( "a." ) );
        ASSERT( !NamespaceString::validCollectionComponent( "a..foo" ) );
        ASSERT( NamespaceString::validCollectionComponent( "a.b." ) ); // TODO: should this change?
    }

    TEST( NamespaceStringTest, CollectionValidNames ) {
        ASSERT( NamespaceString::validCollectionName( "a" ) );
        ASSERT( NamespaceString::validCollectionName( "a.b" ) );
        ASSERT( NamespaceString::validCollectionName( "a." ) ); // TODO: should this change?
        ASSERT( NamespaceString::validCollectionName( "a.b." ) ); // TODO: should this change?
        ASSERT( !NamespaceString::validCollectionName( ".a" ) );
        ASSERT( !NamespaceString::validCollectionName( "$a" ) );
        ASSERT( !NamespaceString::validCollectionName( "a$b" ) );
        ASSERT( !NamespaceString::validCollectionName( "" ) );
        ASSERT( !NamespaceString::validCollectionName(
                    StringData( "a\0b", StringData::LiteralTag() ) ) );
    }

    TEST( NamespaceStringTest, DBHash ) {
        ASSERT_EQUALS( nsDBHash( "foo" ), nsDBHash( "foo" ) );
        ASSERT_EQUALS( nsDBHash( "foo" ), nsDBHash( "foo.a" ) );
        ASSERT_EQUALS( nsDBHash( "foo" ), nsDBHash( "foo." ) );

        ASSERT_EQUALS( nsDBHash( "" ), nsDBHash( "" ) );
        ASSERT_EQUALS( nsDBHash( "" ), nsDBHash( ".a" ) );
        ASSERT_EQUALS( nsDBHash( "" ), nsDBHash( "." ) );

        ASSERT_NOT_EQUALS( nsDBHash( "foo" ), nsDBHash( "food" ) );
        ASSERT_NOT_EQUALS( nsDBHash( "foo." ), nsDBHash( "food" ) );
        ASSERT_NOT_EQUALS( nsDBHash( "foo.d" ), nsDBHash( "food" ) );
    }

#define testEqualsBothWays(X,Y) ASSERT_TRUE( nsDBEquals( (X), (Y) ) ); ASSERT_TRUE( nsDBEquals( (Y), (X) ) );
#define testNotEqualsBothWays(X,Y) ASSERT_FALSE( nsDBEquals( (X), (Y) ) ); ASSERT_FALSE( nsDBEquals( (Y), (X) ) );

    TEST( NamespaceStringTest, DBEquals ) {
        testEqualsBothWays( "foo" , "foo" );
        testEqualsBothWays( "foo" , "foo.a" );
        testEqualsBothWays( "foo.a" , "foo.a" );
        testEqualsBothWays( "foo.a" , "foo.b" );

        testEqualsBothWays( "" , "" );
        testEqualsBothWays( "" , "." );
        testEqualsBothWays( "" , ".x" );

        testNotEqualsBothWays( "foo" , "bar" );
        testNotEqualsBothWays( "foo" , "food" );
        testNotEqualsBothWays( "foo." , "food" );

        testNotEqualsBothWays( "" , "x" );
        testNotEqualsBothWays( "" , "x." );
        testNotEqualsBothWays( "" , "x.y" );
        testNotEqualsBothWays( "." , "x" );
        testNotEqualsBothWays( "." , "x." );
        testNotEqualsBothWays( "." , "x.y" );
    }

    TEST( NamespaceStringTest, nsToDatabase1 ) {
        ASSERT_EQUALS( "foo", nsToDatabaseSubstring( "foo.bar" ) );
        ASSERT_EQUALS( "foo", nsToDatabaseSubstring( "foo" ) );
        ASSERT_EQUALS( "foo", nsToDatabase( "foo.bar" ) );
        ASSERT_EQUALS( "foo", nsToDatabase( "foo" ) );
        ASSERT_EQUALS( "foo", nsToDatabase( string("foo.bar") ) );
        ASSERT_EQUALS( "foo", nsToDatabase( string("foo") ) );
    }

    TEST( NamespaceStringTest, nsToDatabase2 ) {
        char buf[MaxDatabaseNameLen];

        nsToDatabase( "foo.bar", buf );
        ASSERT_EQUALS( 'f', buf[0] );
        ASSERT_EQUALS( 'o', buf[1] );
        ASSERT_EQUALS( 'o', buf[2] );
        ASSERT_EQUALS( 0, buf[3] );

        nsToDatabase( "bar", buf );
        ASSERT_EQUALS( 'b', buf[0] );
        ASSERT_EQUALS( 'a', buf[1] );
        ASSERT_EQUALS( 'r', buf[2] );
        ASSERT_EQUALS( 0, buf[3] );


    }

    TEST( NamespaceStringTest, NamespaceStringParse1 ) {
        NamespaceString ns( "a.b" );
        ASSERT_EQUALS( (string)"a", ns.db() );
        ASSERT_EQUALS( (string)"b", ns.coll() );
    }

    TEST( NamespaceStringTest, NamespaceStringParse2 ) {
        NamespaceString ns( "a.b.c" );
        ASSERT_EQUALS( (string)"a", ns.db() );
        ASSERT_EQUALS( (string)"b.c", ns.coll() );
    }

    TEST( NamespaceStringTest, NamespaceStringParse3 ) {
        NamespaceString ns( "abc" );
        ASSERT_EQUALS( (string)"", ns.db() );
        ASSERT_EQUALS( (string)"", ns.coll() );
    }

    TEST( NamespaceStringTest, NamespaceStringParse4 ) {
        NamespaceString ns( "abc." );
        ASSERT_EQUALS( (string)"abc", ns.db() );
        ASSERT_EQUALS( (string)"", ns.coll() );
    }

}

