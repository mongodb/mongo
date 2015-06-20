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
#include "mongo/db/json.h"
#include "mongo/db/matcher/path.h"

namespace mongo {

using std::string;

TEST(Path, Root1) {
    ElementPath p;
    ASSERT(p.init("a").isOK());

    BSONObj doc = BSON("x" << 4 << "a" << 5);

    BSONElementIterator cursor(&p, doc);
    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS((string) "a", e.element().fieldName());
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT(!cursor.more());
}

TEST(Path, RootArray1) {
    ElementPath p;
    ASSERT(p.init("a").isOK());

    BSONObj doc = BSON("x" << 4 << "a" << BSON_ARRAY(5 << 6));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(6, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());

    ASSERT(!cursor.more());
}

TEST(Path, RootArray2) {
    ElementPath p;
    ASSERT(p.init("a").isOK());
    p.setTraverseLeafArray(false);

    BSONObj doc = BSON("x" << 4 << "a" << BSON_ARRAY(5 << 6));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT(e.element().type() == Array);

    ASSERT(!cursor.more());
}

TEST(Path, Nested1) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());

    BSONObj doc =
        BSON("a" << BSON_ARRAY(BSON("b" << 5) << 3 << BSONObj() << BSON("b" << BSON_ARRAY(9 << 11))
                                              << BSON("b" << 7)));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT_EQUALS((string) "2", e.arrayOffset().fieldName());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(9, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(11, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(2, e.element().Obj().nFields());
    ASSERT(e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(!cursor.more());
}

TEST(Path, NestedPartialMatchScalar) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());

    BSONObj doc = BSON("a" << 4);

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT(e.arrayOffset().eoo());
    ASSERT(!e.outerArray());

    ASSERT(!cursor.more());
}

// When the path (partially or in its entirety) refers to an array,
// the iteration logic does not return an EOO.
// what we want ideally.
TEST(Path, NestedPartialMatchArray) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(4));

    BSONElementIterator cursor(&p, doc);

    ASSERT(!cursor.more());
}

// Note that this describes existing behavior and not necessarily
TEST(Path, NestedEmptyArray) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());

    BSONObj doc = BSON("a" << BSON("b" << BSONArray()));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(0, e.element().Obj().nFields());
    ASSERT(e.outerArray());

    ASSERT(!cursor.more());
}

TEST(Path, NestedNoLeaf1) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());
    p.setTraverseLeafArray(false);

    BSONObj doc =
        BSON("a" << BSON_ARRAY(BSON("b" << 5) << 3 << BSONObj() << BSON("b" << BSON_ARRAY(9 << 11))
                                              << BSON("b" << 7)));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT_EQUALS((string) "2", e.arrayOffset().fieldName());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(2, e.element().Obj().nFields());
    ASSERT(e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(!cursor.more());
}


TEST(Path, ArrayIndex1) {
    ElementPath p;
    ASSERT(p.init("a.1").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << 7 << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndex2) {
    ElementPath p;
    ASSERT(p.init("a.1").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON_ARRAY(2 << 4) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndex3) {
    ElementPath p;
    ASSERT(p.init("a.1").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON("1" << 4) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(4, e.element().numberInt());
    ASSERT(!e.outerArray());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSON("1" << 4), e.element().Obj());
    ASSERT(e.outerArray());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndexNested1) {
    ElementPath p;
    ASSERT(p.init("a.1.b").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON("b" << 4) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT(e.element().eoo());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(4, e.element().numberInt());


    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndexNested2) {
    ElementPath p;
    ASSERT(p.init("a.1.b").isOK());

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON_ARRAY(BSON("b" << 4)) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(4, e.element().numberInt());


    ASSERT(!cursor.more());
}

// SERVER-15899: test iteration using a path that generates no elements, but traverses a long
// array containing subdocuments with nested arrays.
TEST(Path, NonMatchingLongArrayOfSubdocumentsWithNestedArrays) {
    ElementPath p;
    ASSERT(p.init("a.b.x").isOK());

    // Build the document {a: [{b: []}, {b: []}, {b: []}, ...]}.
    BSONObj subdoc = BSON("b" << BSONArray());
    BSONArrayBuilder builder;
    for (int i = 0; i < 100 * 1000; ++i) {
        builder.append(subdoc);
    }
    BSONObj doc = BSON("a" << builder.arr());

    BSONElementIterator cursor(&p, doc);

    // The path "a.b.x" matches no elements.
    ASSERT(!cursor.more());
}

// When multiple arrays are traversed implicitly in the same path,
// ElementIterator::Context::arrayOffset() should always refer to the current offset of the
// outermost array that is implicitly traversed.
TEST(Path, NestedArrayImplicitTraversal) {
    ElementPath p;
    ASSERT(p.init("a.b").isOK());
    BSONObj doc = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(2, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(3, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(BSON("0" << 2 << "1" << 3), e.element().Obj());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(4, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(BSON("0" << 4 << "1" << 5), e.element().Obj());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(!cursor.more());
}

// SERVER-14886: when an array is being traversed explictly at the same time that a nested array
// is being traversed implicitly, ElementIterator::Context::arrayOffset() should return the
// current offset of the array being implicitly traversed.
TEST(Path, ArrayOffsetWithImplicitAndExplicitTraversal) {
    ElementPath p;
    ASSERT(p.init("a.0.b").isOK());
    BSONObj doc = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(EOO, e.element().type());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());  // First elt of outer array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(2, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());  // First elt of inner array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(NumberInt, e.element().type());
    ASSERT_EQUALS(3, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());  // Second elt of inner array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(Array, e.element().type());
    ASSERT_EQUALS(BSON("0" << 2 << "1" << 3), e.element().Obj());
    ASSERT(e.arrayOffset().eoo());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(EOO, e.element().type());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());  // Second elt of outer array.

    ASSERT(!cursor.more());
}

TEST(SimpleArrayElementIterator, SimpleNoArrayLast1) {
    BSONObj obj = BSON("a" << BSON_ARRAY(5 << BSON("x" << 6) << BSON_ARRAY(7 << 9) << 11));
    SimpleArrayElementIterator i(obj["a"], false);

    ASSERT(i.more());
    ElementIterator::Context e = i.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(6, e.element().Obj()["x"].numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(7, e.element().Obj().firstElement().numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(11, e.element().numberInt());

    ASSERT(!i.more());
}

TEST(SimpleArrayElementIterator, SimpleArrayLast1) {
    BSONObj obj = BSON("a" << BSON_ARRAY(5 << BSON("x" << 6) << BSON_ARRAY(7 << 9) << 11));
    SimpleArrayElementIterator i(obj["a"], true);

    ASSERT(i.more());
    ElementIterator::Context e = i.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(6, e.element().Obj()["x"].numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(7, e.element().Obj().firstElement().numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(11, e.element().numberInt());

    ASSERT(i.more());
    e = i.next();
    ASSERT_EQUALS(Array, e.element().type());

    ASSERT(!i.more());
}

TEST(SingleElementElementIterator, Simple1) {
    BSONObj obj = BSON("x" << 3 << "y" << 5);
    SingleElementElementIterator i(obj["y"]);

    ASSERT(i.more());
    ElementIterator::Context e = i.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(!i.more());
}
}
