// namespace_test.h

/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/unittest/unittest.h"

#include "mongo/db/storage/namespace.h"

namespace mongo {

    TEST( NamespaceTest, Basics ) {
        Namespace foo( "foo.bar" );
        Namespace bar( "bar.foo" );

        ASSERT_EQUALS( foo.toString(), foo.toString() );
        ASSERT_EQUALS( foo.hash(), foo.hash() );

        ASSERT_NOT_EQUALS( foo.hash(), bar.hash() );

        ASSERT( foo == foo );
        ASSERT( !( foo != foo ) );
        ASSERT( foo != bar );
        ASSERT( !( foo == bar ) );
    }

    TEST( NamespaceTest, ExtraName ) {
        Namespace foo( "foo.bar" );
        ASSERT_FALSE( foo.isExtra() );

        string str0 = foo.extraName( 0 );
        ASSERT_EQUALS( "foo.bar$extra", str0 );
        Namespace ex0( str0 );
        ASSERT_TRUE( ex0.isExtra() );

        string str1 = foo.extraName( 1 );
        ASSERT_EQUALS( "foo.bar$extrb", str1 );
        Namespace ex1( str1 );
        ASSERT_TRUE( ex1.isExtra() );

    }
}
