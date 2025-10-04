/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/matcher/path.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>

namespace mongo {

using std::string;

TEST(Path, Root1) {
    ElementPath p{"a"};

    BSONObj doc = BSON("x" << 4 << "a" << 5);

    BSONElementIterator cursor(&p, doc);
    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS((string) "a", e.element().fieldName());
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT(!cursor.more());
}

TEST(Path, RootArray1) {
    ElementPath p{"a"};

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
    ASSERT_EQUALS(BSONType::array, e.element().type());

    ASSERT(!cursor.more());
}

TEST(Path, RootArray2) {
    ElementPath p{"a"};
    p.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kNoTraversal);

    BSONObj doc = BSON("x" << 4 << "a" << BSON_ARRAY(5 << 6));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT(e.element().type() == BSONType::array);

    ASSERT(!cursor.more());
}

TEST(Path, Nested1) {
    ElementPath p{"a.b"};

    BSONObj doc =
        BSON("a" << BSON_ARRAY(BSON("b" << 5) << 3 << BSONObj() << BSON("b" << BSON_ARRAY(9 << 11))
                                              << BSON("b" << 7)));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT_EQUALS((string) "2", e.arrayOffset().fieldName());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(9, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(11, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_EQUALS(2, e.element().Obj().nFields());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());

    ASSERT(!cursor.more());
}

TEST(Path, NestedPartialMatchScalar) {
    ElementPath p{"a.b"};

    BSONObj doc = BSON("a" << 4);

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT(e.arrayOffset().eoo());

    ASSERT(!cursor.more());
}

// When the path (partially or in its entirety) refers to an array,
// the iteration logic does not return an EOO.
// what we want ideally.
TEST(Path, NestedPartialMatchArray) {
    ElementPath p{"a.b"};

    BSONObj doc = BSON("a" << BSON_ARRAY(4));

    BSONElementIterator cursor(&p, doc);

    ASSERT(!cursor.more());
}

// Note that this describes existing behavior and not necessarily
TEST(Path, NestedEmptyArray) {
    ElementPath p{"a.b"};

    BSONObj doc = BSON("a" << BSON("b" << BSONArray()));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_EQUALS(0, e.element().Obj().nFields());

    ASSERT(!cursor.more());
}

TEST(Path, NestedNoLeaf1) {
    ElementPath p{"a.b"};
    p.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kNoTraversal);

    BSONObj doc =
        BSON("a" << BSON_ARRAY(BSON("b" << 5) << 3 << BSONObj() << BSON("b" << BSON_ARRAY(9 << 11))
                                              << BSON("b" << 7)));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(5, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT(e.element().eoo());
    ASSERT_EQUALS((string) "2", e.arrayOffset().fieldName());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_EQUALS(2, e.element().Obj().nFields());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());

    ASSERT(!cursor.more());
}

TEST(Path, MatchSubpathReturnsArrayOnSubpath) {
    ElementPath path{"a.b.c"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kNoTraversal);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kMatchSubpath);

    BSONObj doc = BSON("a" << BSON_ARRAY(BSON("b" << 5)));

    BSONElementIterator cursor(&path, doc);

    ASSERT(cursor.more());
    auto context = cursor.next();
    ASSERT_BSONELT_EQ(doc.firstElement(), context.element());

    ASSERT(!cursor.more());
}

TEST(Path, MatchSubpathWithTraverseLeafFalseReturnsLeafArrayOnPath) {
    ElementPath path{"a.b.c"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kNoTraversal);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kMatchSubpath);

    BSONObj doc = BSON("a" << BSON("b" << BSON("c" << BSON_ARRAY(1 << 2))));

    BSONElementIterator cursor(&path, doc);

    ASSERT(cursor.more());
    auto context = cursor.next();
    ASSERT_BSONELT_EQ(fromjson("{c: [1, 2]}").firstElement(), context.element());

    ASSERT(!cursor.more());
}

TEST(Path, MatchSubpathWithTraverseLeafTrueReturnsLeafArrayAndValuesOnPath) {
    ElementPath path{"a.b.c"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kTraverse);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kMatchSubpath);

    BSONObj doc = BSON("a" << BSON("b" << BSON("c" << BSON_ARRAY(1 << 2))));

    BSONElementIterator cursor(&path, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context context = cursor.next();
    ASSERT_EQUALS(1, context.element().numberInt());

    ASSERT(cursor.more());
    context = cursor.next();
    ASSERT_EQUALS(2, context.element().numberInt());

    ASSERT(cursor.more());
    context = cursor.next();
    ASSERT_BSONELT_EQ(fromjson("{c: [1, 2]}").firstElement(), context.element());

    ASSERT(!cursor.more());
}

TEST(Path, MatchSubpathWithMultipleArraysReturnsOutermostArray) {
    ElementPath path{"a.b.c"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kTraverse);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kMatchSubpath);

    BSONObj doc = fromjson("{a: [{b: [{c: [1]}]}]}");

    BSONElementIterator cursor(&path, doc);

    ASSERT(cursor.more());
    auto context = cursor.next();
    ASSERT_BSONELT_EQ(fromjson("{a: [{b: [{c: [1]}]}]}").firstElement(), context.element());

    ASSERT(!cursor.more());
}

TEST(Path, NoTraversalOfNonLeafArrayReturnsNothingWithNonLeafArrayInDoc) {
    ElementPath path{"a.b"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kTraverse);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kNoTraversal);

    BSONObj doc = fromjson("{a: [{b: 1}]}");

    BSONElementIterator cursor(&path, doc);
    ASSERT(!cursor.more());
}

TEST(Path, MatchSubpathWithNumericalPathComponentReturnsEntireArray) {
    ElementPath path{"a.0.b"};
    path.setLeafArrayBehavior(ElementPath::LeafArrayBehavior::kTraverse);
    path.setNonLeafArrayBehavior(ElementPath::NonLeafArrayBehavior::kMatchSubpath);

    BSONObj doc = fromjson("{a: [{b: 1}]}");

    BSONElementIterator cursor(&path, doc);

    ASSERT(cursor.more());
    auto context = cursor.next();
    ASSERT_BSONELT_EQ(fromjson("{a: [{b: 1}]}").firstElement(), context.element());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndex1) {
    ElementPath p{"a.1"};

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << 7 << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(7, e.element().numberInt());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndex2) {
    ElementPath p{"a.1"};

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON_ARRAY(2 << 4) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndex3) {
    ElementPath p{"a.1"};

    BSONObj doc = BSON("a" << BSON_ARRAY(5 << BSON("1" << 4) << 3));

    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    BSONElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(4, e.element().numberInt());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_BSONOBJ_EQ(BSON("1" << 4), e.element().Obj());

    ASSERT(!cursor.more());
}

TEST(Path, ArrayIndexNested1) {
    ElementPath p{"a.1.b"};

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
    ElementPath p{"a.1.b"};

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
    ElementPath p{"a.b.x"};

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
    ElementPath p{"a.b"};
    BSONObj doc = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(2, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(3, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_BSONOBJ_EQ(BSON("0" << 2 << "1" << 3), e.element().Obj());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(4, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(5, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_BSONOBJ_EQ(BSON("0" << 4 << "1" << 5), e.element().Obj());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());

    ASSERT(!cursor.more());
}

// SERVER-14886: when an array is being traversed explictly at the same time that a nested array
// is being traversed implicitly, ElementIterator::Context::arrayOffset() should return the
// current offset of the array being implicitly traversed.
TEST(Path, ArrayOffsetWithImplicitAndExplicitTraversal) {
    ElementPath p{"a.0.b"};
    BSONObj doc = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    BSONElementIterator cursor(&p, doc);

    ASSERT(cursor.more());
    ElementIterator::Context e = cursor.next();
    ASSERT_EQUALS(BSONType::eoo, e.element().type());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());  // First elt of outer array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(2, e.element().numberInt());
    ASSERT_EQUALS("0", e.arrayOffset().fieldNameStringData());  // First elt of inner array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::numberInt, e.element().type());
    ASSERT_EQUALS(3, e.element().numberInt());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());  // Second elt of inner array.

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::array, e.element().type());
    ASSERT_BSONOBJ_EQ(BSON("0" << 2 << "1" << 3), e.element().Obj());
    ASSERT(e.arrayOffset().eoo());

    ASSERT(cursor.more());
    e = cursor.next();
    ASSERT_EQUALS(BSONType::eoo, e.element().type());
    ASSERT_EQUALS("1", e.arrayOffset().fieldNameStringData());  // Second elt of outer array.

    ASSERT(!cursor.more());
}

TEST(Path, LeafArrayBehaviorTraverseOmitArrayWithNonEmptyArray) {
    ElementPath path{"a", ElementPath::LeafArrayBehavior::kTraverseOmitArray};
    BSONObj doc = fromjson("{a: [1, 2]}");
    BSONElementIterator cursor(&path, doc);

    // Verifies that only array elements are returned by the iterator, that is the array [1, 2] is
    // not returned.
    ASSERT_TRUE(cursor.more());
    ElementIterator::Context context = cursor.next();
    ASSERT_EQUALS(1, context.element().Int());

    ASSERT_TRUE(cursor.more());
    context = cursor.next();
    ASSERT_EQUALS(2, context.element().Int());

    ASSERT_FALSE(cursor.more());
}

TEST(Path, LeafArrayBehaviorTraverseOmitArrayWithEmptyArray) {
    ElementPath path{"a", ElementPath::LeafArrayBehavior::kTraverseOmitArray};
    BSONObj doc = fromjson("{a: []}");
    BSONElementIterator cursor(&path, doc);

    // Verifies that no elements are returned by the iterator since the array is empty.
    ASSERT_FALSE(cursor.more());
}

TEST(Path, LeafArrayBehaviorTraverseOmitArrayNested) {
    ElementPath path{"a.b", ElementPath::LeafArrayBehavior::kTraverseOmitArray};
    BSONObj doc = fromjson("{a: [{b: [1]}, {b: []}, {b: [2, 3]}]}");
    BSONElementIterator cursor(&path, doc);

    // Verifies that all elements of nested arrays are returned.
    for (auto&& element : {1, 2, 3}) {
        ASSERT_TRUE(cursor.more());
        ASSERT_EQUALS(element, cursor.next().element().Int());
    }
    ASSERT_FALSE(cursor.more());
}

TEST(Path, LeafArrayBehaviorTraverseOmitArrayNestedEmptyArray) {
    ElementPath path{"a.b", ElementPath::LeafArrayBehavior::kTraverseOmitArray};
    BSONObj doc = fromjson("{a: [{b: []}, {b: []}]}");
    BSONElementIterator cursor(&path, doc);

    // Verifies that no elements are returned.
    ASSERT_FALSE(cursor.more());
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
    ASSERT_EQUALS(BSONType::array, e.element().type());

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
}  // namespace mongo
