// path_test.cpp


/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/path.h"

namespace mongo {

    TEST( Path, Root1 ) {
        ElementPath p;
        ASSERT( p.init( "a" ).isOK() );

        BSONObj doc = BSON( "x" << 4 << "a" << 5 );

        BSONElementIterator cursor( &p, doc );
        ASSERT( cursor.more() );
        ElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( (string)"a", e.element().fieldName() );
        ASSERT_EQUALS( 5, e.element().numberInt() );
        ASSERT( !cursor.more() );
    }

    TEST( Path, RootArray1 ) {
        ElementPath p;
        ASSERT( p.init( "a" ).isOK() );

        BSONObj doc = BSON( "x" << 4 << "a" << BSON_ARRAY( 5 << 6 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 6, e.element().numberInt() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( Array, e.element().type() );

        ASSERT( !cursor.more() );
    }

    TEST( Path, RootArray2 ) {
        ElementPath p;
        ASSERT( p.init( "a" ).isOK() );
        p.setTraverseLeafArray( false );

        BSONObj doc = BSON( "x" << 4 << "a" << BSON_ARRAY( 5 << 6 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT( e.element().type() == Array );

        ASSERT( !cursor.more() );
    }

    TEST( Path, Nested1 ) {
        ElementPath p;
        ASSERT( p.init( "a.b" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( BSON( "b" << 5 ) <<
                                               3 <<
                                               BSONObj() <<
                                               BSON( "b" << BSON_ARRAY( 9 << 11 ) ) <<
                                               BSON( "b" << 7 ) ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT( e.element().eoo() );
        ASSERT_EQUALS( (string)"2", e.arrayOffset().fieldName() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 9, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 11, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( Array, e.element().type() );
        ASSERT_EQUALS( 2, e.element().Obj().nFields() );
        ASSERT( e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 7, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( !cursor.more() );
    }

    TEST( Path, NestedNoLeaf1 ) {
        ElementPath p;
        ASSERT( p.init( "a.b" ).isOK() );
        p.setTraverseLeafArray( false );

        BSONObj doc = BSON( "a" << BSON_ARRAY( BSON( "b" << 5 ) <<
                                               3 <<
                                               BSONObj() <<
                                               BSON( "b" << BSON_ARRAY( 9 << 11 ) ) <<
                                               BSON( "b" << 7 ) ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT( e.element().eoo() );
        ASSERT_EQUALS( (string)"2", e.arrayOffset().fieldName() );
        ASSERT( !e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( Array, e.element().type() );
        ASSERT_EQUALS( 2, e.element().Obj().nFields() );
        ASSERT( e.outerArray() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 7, e.element().numberInt() );
        ASSERT( !e.outerArray() );

        ASSERT( !cursor.more() );
    }


    TEST( Path, ArrayIndex1 ) {
        ElementPath p;
        ASSERT( p.init( "a.1" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( 5 << 7 << 3 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 7, e.element().numberInt() );

        ASSERT( !cursor.more() );
    }

    TEST( Path, ArrayIndex2 ) {
        ElementPath p;
        ASSERT( p.init( "a.1" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( 5 << BSON_ARRAY( 2 << 4 ) << 3 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( Array, e.element().type() );

        ASSERT( !cursor.more() );
    }

    TEST( Path, ArrayIndex3 ) {
        ElementPath p;
        ASSERT( p.init( "a.1" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( 5 << BSON( "1" << 4 ) << 3 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 4, e.element().numberInt() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( BSON( "1" << 4 ), e.element().Obj() );

        ASSERT( !cursor.more() );
    }

    TEST( Path, ArrayIndexNested1 ) {
        ElementPath p;
        ASSERT( p.init( "a.1.b" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( 5 << BSON( "b" << 4 ) << 3 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT( e.element().eoo() );

        ASSERT( cursor.more() );
        e = cursor.next();
        ASSERT_EQUALS( 4, e.element().numberInt() );


        ASSERT( !cursor.more() );
    }

    TEST( Path, ArrayIndexNested2 ) {
        ElementPath p;
        ASSERT( p.init( "a.1.b" ).isOK() );

        BSONObj doc = BSON( "a" << BSON_ARRAY( 5 << BSON_ARRAY( BSON( "b" << 4 ) ) << 3 ) );

        BSONElementIterator cursor( &p, doc );

        ASSERT( cursor.more() );
        BSONElementIterator::Context e = cursor.next();
        ASSERT_EQUALS( 4, e.element().numberInt() );


        ASSERT( !cursor.more() );
    }

    TEST( SimpleArrayElementIterator, SimpleNoArrayLast1 ) {
        BSONObj obj = BSON( "a" << BSON_ARRAY( 5 << BSON( "x" << 6 ) << BSON_ARRAY( 7 << 9 ) << 11 ) );
        SimpleArrayElementIterator i( obj["a"], false );

        ASSERT( i.more() );
        ElementIterator::Context e = i.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 6, e.element().Obj()["x"].numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 7, e.element().Obj().firstElement().numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 11, e.element().numberInt() );

        ASSERT( !i.more() );
    }

    TEST( SimpleArrayElementIterator, SimpleArrayLast1 ) {
        BSONObj obj = BSON( "a" << BSON_ARRAY( 5 << BSON( "x" << 6 ) << BSON_ARRAY( 7 << 9 ) << 11 ) );
        SimpleArrayElementIterator i( obj["a"], true );

        ASSERT( i.more() );
        ElementIterator::Context e = i.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 6, e.element().Obj()["x"].numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 7, e.element().Obj().firstElement().numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( 11, e.element().numberInt() );

        ASSERT( i.more() );
        e = i.next();
        ASSERT_EQUALS( Array, e.element().type() );

        ASSERT( !i.more() );
    }

    TEST( SingleElementElementIterator, Simple1 ) {
        BSONObj obj = BSON( "x" << 3 << "y" << 5 );
        SingleElementElementIterator i( obj["y"] );

        ASSERT( i.more() );
        ElementIterator::Context e = i.next();
        ASSERT_EQUALS( 5, e.element().numberInt() );

        ASSERT( !i.more() );

    }

}
