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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace mongo;
namespace mmb = mongo::mutablebson;

TEST(TopologyBuilding, TopDownFromScratch) {
    /*
                   [ e0 ]
                    /   \
                   /     \
               [ e1 ]..[ e2 ]
                        /
                       /
                   [ e3 ]
                    /   \
                   /     \
               [ e4 ]..[ e5 ]
    */

    mmb::Document doc;

    mmb::Element e0 = doc.makeElementObject("e0");
    mmb::Element e1 = doc.makeElementObject("e1");
    mmb::Element e2 = doc.makeElementObject("e2");
    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");
    mmb::Element e5 = doc.makeElementObject("e5");

    ASSERT_EQUALS(e0.pushBack(e1), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e2.pushBack(e3), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e5), mongo::Status::OK());

    ASSERT_EQUALS("e0", e0.getFieldName());
    ASSERT_EQUALS("e1", e0.leftChild().getFieldName());
    ASSERT_EQUALS("e2", e0.rightChild().getFieldName());
    ASSERT_EQUALS("e0", e1.parent().getFieldName());
    ASSERT_EQUALS("e0", e2.parent().getFieldName());
    ASSERT_EQUALS("e2", e1.rightSibling().getFieldName());
    ASSERT_EQUALS("e1", e2.leftSibling().getFieldName());
    ASSERT_EQUALS("e3", e2.leftChild().getFieldName());
    ASSERT_EQUALS("e3", e2.rightChild().getFieldName());

    ASSERT_EQUALS("e2", e3.parent().getFieldName());
    ASSERT_EQUALS("e4", e3.leftChild().getFieldName());
    ASSERT_EQUALS("e5", e3.rightChild().getFieldName());
    ASSERT_EQUALS("e4", e5.leftSibling().getFieldName());
    ASSERT_EQUALS("e5", e4.rightSibling().getFieldName());
    ASSERT_EQUALS("e3", e4.parent().getFieldName());
    ASSERT_EQUALS("e3", e5.parent().getFieldName());
}

TEST(TopologyBuilding, AddSiblingAfter) {
    /*
                   [ e0 ]
                    /   \
                   /     \
               [ e1 ]..[ e2 ]
                        /  \
                       /    \
                   [ e3 ]..[ e4 ]
    */

    mmb::Document doc;

    mmb::Element e0 = doc.makeElementObject("e0");
    mmb::Element e1 = doc.makeElementObject("e1");
    mmb::Element e2 = doc.makeElementObject("e2");
    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");

    ASSERT_EQUALS(e0.pushBack(e1), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e2.pushBack(e3), mongo::Status::OK());
    ASSERT_EQUALS(e3.addSiblingRight(e4), mongo::Status::OK());

    ASSERT_EQUALS("e4", e3.rightSibling().getFieldName());
    ASSERT_EQUALS("e3", e4.leftSibling().getFieldName());
    ASSERT_EQUALS("e2", e4.parent().getFieldName());
    ASSERT_EQUALS("e3", e2.leftChild().getFieldName());
    ASSERT_EQUALS("e4", e2.rightChild().getFieldName());
}

TEST(TopologyBuilding, AddSiblingBefore) {
    /*
                       [ e2 ]
                        /  \
                       /    \
                   [ e3 ]..[ e4 ]
                  /  |   \
                /    |     \
           [ e7 ]..[ e5 ]..[ e6 ]
    */

    mmb::Document doc;

    mmb::Element e2 = doc.makeElementObject("e2");
    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");
    mmb::Element e5 = doc.makeElementObject("e5");
    mmb::Element e6 = doc.makeElementObject("e6");
    mmb::Element e7 = doc.makeElementObject("e7");

    ASSERT_EQUALS(e2.pushBack(e3), mongo::Status::OK());
    ASSERT_EQUALS(e2.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e5), mongo::Status::OK());
    ASSERT_EQUALS(e5.addSiblingRight(e6), mongo::Status::OK());
    ASSERT_EQUALS(e5.addSiblingLeft(e7), mongo::Status::OK());

    ASSERT_EQUALS("e5", e7.rightSibling().getFieldName());
    ASSERT_EQUALS("e7", e5.leftSibling().getFieldName());
    ASSERT_EQUALS("e6", e5.rightSibling().getFieldName());
    ASSERT_EQUALS("e5", e6.leftSibling().getFieldName());

    ASSERT_EQUALS("e3", e5.parent().getFieldName());
    ASSERT_EQUALS("e3", e6.parent().getFieldName());
    ASSERT_EQUALS("e3", e7.parent().getFieldName());
    ASSERT_EQUALS("e7", e3.leftChild().getFieldName());
    ASSERT_EQUALS("e6", e3.rightChild().getFieldName());
}

TEST(TopologyBuilding, AddSubtreeBottomUp) {
    /*
                   [ e3 ]
                  /  |   \
                /    |     \
           [ e4 ]..[ e5 ]..[ e6 ]
                     |
                     |
                   [ e8 ]
                    /  \
                   /    \
               [ e9 ]..[ e10]
    */
    mmb::Document doc;

    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");
    mmb::Element e5 = doc.makeElementObject("e5");
    mmb::Element e6 = doc.makeElementObject("e6");
    mmb::Element e8 = doc.makeElementObject("e8");
    mmb::Element e9 = doc.makeElementObject("e9");
    mmb::Element e10 = doc.makeElementObject("e10");

    ASSERT_EQUALS(e3.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e5), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e6), mongo::Status::OK());
    ASSERT_EQUALS(e8.pushBack(e9), mongo::Status::OK());
    ASSERT_EQUALS(e8.pushBack(e10), mongo::Status::OK());
    ASSERT_EQUALS(e5.pushBack(e8), mongo::Status::OK());

    ASSERT_EQUALS("e8", e9.parent().getFieldName());
    ASSERT_EQUALS("e8", e10.parent().getFieldName());
    ASSERT_EQUALS("e9", e8.leftChild().getFieldName());
    ASSERT_EQUALS("e10", e8.rightChild().getFieldName());
    ASSERT_EQUALS("e9", e10.leftSibling().getFieldName());
    ASSERT_EQUALS("e10", e9.rightSibling().getFieldName());
    ASSERT_EQUALS("e5", e8.parent().getFieldName());
    ASSERT_EQUALS("e8", e5.leftChild().getFieldName());
    ASSERT_EQUALS("e8", e5.rightChild().getFieldName());
}

TEST(TopologyBuilding, RemoveLeafNode) {
    /*
                   [ e0 ]                [ e0 ]
                    /   \       =>          \
                   /     \                   \
               [ e1 ]   [ e2 ]              [ e2 ]
    */
    mmb::Document doc;

    mmb::Element e0 = doc.makeElementObject("e0");
    mmb::Element e1 = doc.makeElementObject("e1");
    mmb::Element e2 = doc.makeElementObject("e2");

    ASSERT_EQUALS(e0.pushBack(e1), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e1.remove(), mongo::Status::OK());

    ASSERT_EQUALS("e2", e0.leftChild().getFieldName());
    ASSERT_EQUALS("e2", e0.rightChild().getFieldName());
}

TEST(TopologyBuilding, RemoveSubtree) {
    /*
                   [ e3 ]                               [ e3 ]
                  /  |   \                               /  \
                /    |     \                            /    \
           [ e4 ]..[ e5 ]..[ e6 ]                   [ e4 ]  [ e6 ]
                     |                  =>
                     |
                   [ e8 ]
                    /  \
                   /    \
               [ e9 ]..[ e10]
    */
    mmb::Document doc;

    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");
    mmb::Element e5 = doc.makeElementObject("e5");
    mmb::Element e6 = doc.makeElementObject("e6");
    mmb::Element e8 = doc.makeElementObject("e8");
    mmb::Element e9 = doc.makeElementObject("e9");
    mmb::Element e10 = doc.makeElementObject("e10");

    ASSERT_EQUALS(e3.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e5), mongo::Status::OK());
    ASSERT_EQUALS(e5.addSiblingRight(e6), mongo::Status::OK());

    ASSERT_EQUALS(e8.pushBack(e9), mongo::Status::OK());
    ASSERT_EQUALS(e8.pushBack(e10), mongo::Status::OK());
    ASSERT_EQUALS(e5.pushBack(e8), mongo::Status::OK());
    ASSERT_EQUALS(e5.remove(), mongo::Status::OK());

    ASSERT_EQUALS("e3", e4.parent().getFieldName());
    ASSERT_EQUALS("e3", e6.parent().getFieldName());
    ASSERT_EQUALS("e4", e3.leftChild().getFieldName());
    ASSERT_EQUALS("e6", e3.rightChild().getFieldName());
}

TEST(TopologyBuilding, RenameNode) {
    /*

                   [ e0 ]                  [ f0 ]
                    /  \          =>        /  \
                   /    \                  /    \
               [ e1 ]..[ e2 ]          [ e1 ]..[ e2 ]
    */

    mmb::Document doc;

    mmb::Element e0 = doc.makeElementObject("e0");
    mmb::Element e1 = doc.makeElementObject("e1");
    mmb::Element e2 = doc.makeElementObject("e2");

    ASSERT_EQUALS(e0.pushBack(e1), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e0.rename("f0"), mongo::Status::OK());
    ASSERT_EQUALS("f0", e0.getFieldName());
}

TEST(TopologyBuilding, MoveNode) {
    /*
                   [ e0 ]                       [ e0 ]
                    /   \                      /  |   \
                   /     \                   /    |     \
               [ e1 ]..[ e2 ]           [ e1 ]..[ e2 ]..[ e3 ]
                        /         =>                     /  \
                       /                                /    \
                   [ e3 ]                           [ e4 ]..[ e5 ]
                    /   \
                   /     \
               [ e4 ]..[ e5 ]
    */

    mmb::Document doc;

    mmb::Element e0 = doc.makeElementObject("e0");
    mmb::Element e1 = doc.makeElementObject("e1");
    mmb::Element e2 = doc.makeElementObject("e2");
    mmb::Element e3 = doc.makeElementObject("e3");
    mmb::Element e4 = doc.makeElementObject("e4");
    mmb::Element e5 = doc.makeElementObject("e5");

    ASSERT_EQUALS(e0.pushBack(e1), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e2.pushBack(e3), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e3.pushBack(e5), mongo::Status::OK());

    ASSERT_EQUALS("e0", e0.getFieldName());
    ASSERT_EQUALS("e1", e0.leftChild().getFieldName());
    ASSERT_EQUALS("e2", e0.rightChild().getFieldName());
    ASSERT_EQUALS("e0", e1.parent().getFieldName());
    ASSERT_EQUALS("e0", e2.parent().getFieldName());
    ASSERT_EQUALS("e2", e1.rightSibling().getFieldName());
    ASSERT_EQUALS("e1", e2.leftSibling().getFieldName());
    ASSERT_EQUALS("e3", e2.leftChild().getFieldName());
    ASSERT_EQUALS("e3", e2.rightChild().getFieldName());
    ASSERT_EQUALS("e4", e3.leftChild().getFieldName());
    ASSERT_EQUALS("e5", e3.rightChild().getFieldName());
    ASSERT_EQUALS("e5", e4.rightSibling().getFieldName());
    ASSERT_EQUALS("e4", e5.leftSibling().getFieldName());

    ASSERT_EQUALS(e3.remove(), mongo::Status::OK());
    ASSERT_EQUALS(e0.pushBack(e3), mongo::Status::OK());

    ASSERT_EQUALS("e0", e3.parent().getFieldName());
    ASSERT_EQUALS("e1", e0.leftChild().getFieldName());
    ASSERT_EQUALS("e3", e0.rightChild().getFieldName());
    ASSERT_EQUALS("e3", e2.rightSibling().getFieldName());
    ASSERT_EQUALS("e2", e3.leftSibling().getFieldName());
    ASSERT_EQUALS("e4", e3.leftChild().getFieldName());
    ASSERT_EQUALS("e5", e3.rightChild().getFieldName());
}

TEST(TopologyBuilding, CantAddAttachedAsLeftSibling) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("foo", "foo"));
    mmb::Element foo = doc.root().rightChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(doc.root().appendString("bar", "bar"));
    mmb::Element bar = doc.root().rightChild();
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.addSiblingLeft(bar));
}

TEST(TopologyBuilding, CantAddAttachedAsRightSibling) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("foo", "foo"));
    mmb::Element foo = doc.root().rightChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(doc.root().appendString("bar", "bar"));
    mmb::Element bar = doc.root().rightChild();
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.addSiblingRight(bar));
}

TEST(TopologyBuilding, CantAddAttachedAsChild) {
    mmb::Document doc;
    mmb::Element foo = doc.makeElementObject("foo");
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(doc.root().pushBack(foo));
    ASSERT_OK(doc.root().appendString("bar", "bar"));
    mmb::Element bar = doc.root().rightChild();
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.pushFront(bar));
    ASSERT_NOT_OK(foo.pushBack(bar));
}

TEST(TopologyBuilding, CantAddChildrenToNonObject) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("foo", "bar"));
    mmb::Element foo = doc.root().rightChild();
    ASSERT_TRUE(foo.ok());
    mmb::Element bar = doc.makeElementString("bar", "bar");
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.pushFront(bar));
    ASSERT_NOT_OK(foo.pushBack(bar));
}

TEST(TopologyBuilding, CantAddLeftSiblingToDetached) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("foo", "foo"));
    mmb::Element foo = doc.root().rightChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(foo.remove());
    ASSERT_FALSE(foo.parent().ok());
    mmb::Element bar = doc.makeElementString("bar", "bar");
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.addSiblingLeft(bar));
}

TEST(TopologyBuilding, CantAddRightSiblingToDetached) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("foo", "foo"));
    mmb::Element foo = doc.root().rightChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(foo.remove());
    ASSERT_FALSE(foo.parent().ok());
    mmb::Element bar = doc.makeElementString("bar", "bar");
    ASSERT_TRUE(bar.ok());
    ASSERT_NOT_OK(foo.addSiblingRight(bar));
}

TEST(TopologyBuilding, AddSiblingLeftIntrusion) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("first", "first"));
    mmb::Element first = doc.root().rightChild();
    ASSERT_TRUE(first.ok());
    ASSERT_OK(doc.root().appendString("last", "last"));
    mmb::Element last = doc.root().rightChild();
    ASSERT_TRUE(last.ok());

    ASSERT_EQUALS(first, last.leftSibling());
    ASSERT_EQUALS(last, first.rightSibling());

    mmb::Element middle = doc.makeElementString("middle", "middle");
    ASSERT_TRUE(middle.ok());
    ASSERT_OK(last.addSiblingLeft(middle));

    ASSERT_EQUALS(middle, first.rightSibling());
    ASSERT_EQUALS(middle, last.leftSibling());

    ASSERT_EQUALS(first, middle.leftSibling());
    ASSERT_EQUALS(last, middle.rightSibling());
}

TEST(TopologyBuilding, AddSiblingRightIntrusion) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendString("first", "first"));
    mmb::Element first = doc.root().rightChild();
    ASSERT_TRUE(first.ok());
    ASSERT_OK(doc.root().appendString("last", "last"));
    mmb::Element last = doc.root().rightChild();
    ASSERT_TRUE(last.ok());
    mmb::Element middle = doc.makeElementString("middle", "middle");
    ASSERT_TRUE(middle.ok());
    ASSERT_OK(first.addSiblingRight(middle));
    ASSERT_EQUALS(first, middle.leftSibling());
    ASSERT_EQUALS(last, middle.rightSibling());
}

TEST(ArrayAPI, SimpleNumericArray) {
    /*
            { a : [] }                  create
            { a : [10] }                 pushBack
            { a : [10, 20] }              pushBack
            { a : [10, 20, 30] }           pushBack
            { a : [10, 20, 30, 40] }        pushBack
            { a : [5, 10, 20, 30, 40] }      pushFront
            { a : [0, 5, 10, 20, 30, 40] }    pushFront
            { a : [0, 5, 10, 20, 30] }       popBack
            { a : [5, 10, 20, 30] }         popFront
            { a : [10, 20, 30] }           popFront
            { a : [10, 20] }              popBack
            { a : [20] }                 popFront
            { a : [100] }                set
            { a : [] }                  popFront
    */

    mmb::Document doc;

    mmb::Element e1 = doc.makeElementArray("a");
    ASSERT_FALSE(e1[0].ok());
    ASSERT_FALSE(e1[1].ok());

    mmb::Element e2 = doc.makeElementInt("", 10);
    ASSERT_EQUALS(10, e2.getValueInt());
    mmb::Element e3 = doc.makeElementInt("", 20);
    ASSERT_EQUALS(20, e3.getValueInt());
    mmb::Element e4 = doc.makeElementInt("", 30);
    ASSERT_EQUALS(30, e4.getValueInt());
    mmb::Element e5 = doc.makeElementInt("", 40);
    ASSERT_EQUALS(40, e5.getValueInt());
    mmb::Element e6 = doc.makeElementInt("", 5);
    ASSERT_EQUALS(5, e6.getValueInt());
    mmb::Element e7 = doc.makeElementInt("", 0);
    ASSERT_EQUALS(0, e7.getValueInt());

    ASSERT_EQUALS(e1.pushBack(e2), mongo::Status::OK());
    ASSERT_EQUALS(e1.pushBack(e3), mongo::Status::OK());
    ASSERT_EQUALS(e1.pushBack(e4), mongo::Status::OK());
    ASSERT_EQUALS(e1.pushBack(e5), mongo::Status::OK());
    ASSERT_EQUALS(e1.pushFront(e6), mongo::Status::OK());
    ASSERT_EQUALS(e1.pushFront(e7), mongo::Status::OK());

    ASSERT_EQUALS(size_t(6), mmb::countChildren(e1));
    ASSERT_EQUALS(0, e1[0].getValueInt());
    ASSERT_EQUALS(5, e1[1].getValueInt());
    ASSERT_EQUALS(10, e1[2].getValueInt());
    ASSERT_EQUALS(20, e1[3].getValueInt());
    ASSERT_EQUALS(30, e1[4].getValueInt());
    ASSERT_EQUALS(40, e1[5].getValueInt());
    ASSERT_EQUALS(40, e1.rightChild().getValueInt());
    ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(5), mmb::countChildren(e1));
    ASSERT_EQUALS(0, e1[0].getValueInt());
    ASSERT_EQUALS(5, e1[1].getValueInt());
    ASSERT_EQUALS(10, e1[2].getValueInt());
    ASSERT_EQUALS(20, e1[3].getValueInt());
    ASSERT_EQUALS(30, e1[4].getValueInt());
    ASSERT_EQUALS(0, e1.leftChild().getValueInt());
    ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(4), mmb::countChildren(e1));
    ASSERT_EQUALS(5, e1[0].getValueInt());
    ASSERT_EQUALS(10, e1[1].getValueInt());
    ASSERT_EQUALS(20, e1[2].getValueInt());
    ASSERT_EQUALS(30, e1[3].getValueInt());
    ASSERT_EQUALS(5, e1.leftChild().getValueInt());
    ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(3), mmb::countChildren(e1));
    ASSERT_EQUALS(10, e1[0].getValueInt());
    ASSERT_EQUALS(20, e1[1].getValueInt());
    ASSERT_EQUALS(30, e1[2].getValueInt());
    ASSERT_EQUALS(30, e1.rightChild().getValueInt());
    ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(2), mmb::countChildren(e1));
    ASSERT_EQUALS(10, e1[0].getValueInt());
    ASSERT_EQUALS(20, e1[1].getValueInt());
    ASSERT_EQUALS(10, e1.leftChild().getValueInt());
    ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(1), mmb::countChildren(e1));
    ASSERT_EQUALS(20, e1[0].getValueInt());

    ASSERT_EQUALS(e1[0].setValueInt(100), mongo::Status::OK());
    ASSERT_EQUALS(100, e1[0].getValueInt());
    ASSERT_EQUALS(100, e1.leftChild().getValueInt());
    ASSERT_EQUALS(size_t(1), mmb::countChildren(e1));
    ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());

    ASSERT_EQUALS(size_t(0), mmb::countChildren(e1));
    ASSERT_FALSE(e1[0].ok());
    ASSERT_FALSE(e1[1].ok());
}

DEATH_TEST_REGEX(ArrayAPI,
                 FindFirstChildNamedOnDeserializedArray,
                 R"#(Invariant failure.*getType\(\) != BSONType::array)#") {
    mmb::Document doc;
    auto array = doc.makeElementArray("a");
    auto elem0 = doc.makeElementInt("", 0);
    ASSERT_OK(array.pushBack(elem0));
    array.findFirstChildNamed("0");
}

DEATH_TEST_REGEX(ArrayAPI,
                 FindFirstChildNamedOnSerializedArray,
                 R"#(Invariant failure.*getType\(\) != BSONType::array)#") {
    auto obj = fromjson("{a: [0, 1]}");
    mmb::Document doc(obj);
    auto rootElem = doc.root();
    auto array = rootElem.leftChild();
    array.findFirstChildNamed("0");
}

TEST(Element, setters) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementNull("t0");

    t0.setValueBool(true).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::boolean, t0.getType());

    t0.setValueInt(12345).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::numberInt, t0.getType());

    t0.setValueLong(12345LL).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::numberLong, t0.getType());

    t0.setValueTimestamp(mongo::Timestamp()).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::timestamp, t0.getType());

    t0.setValueDate(mongo::Date_t::fromMillisSinceEpoch(12345LL)).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::date, t0.getType());

    t0.setValueDouble(123.45).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::numberDouble, t0.getType());

    t0.setValueDecimal(mongo::Decimal128("123.45E1234")).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::numberDecimal, t0.getType());

    t0.setValueOID(mongo::OID("47cc67093475061e3d95369d")).transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::oid, t0.getType());

    t0.setValueRegex("[a-zA-Z]?", "").transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::regEx, t0.getType());

    t0.setValueString("foo bar baz").transitional_ignore();
    ASSERT_EQUALS(mongo::BSONType::string, t0.getType());
}

TEST(Element, toString) {
    mongo::BSONObj obj = mongo::fromjson("{ a : 1, b : [1, 2, 3], c : { x : 'x' } }");
    mmb::Document doc(obj);

    // Deserialize the 'c' but keep its value the same.
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    ASSERT_OK(c.appendString("y", "y"));
    ASSERT_OK(c.popBack());

    // 'a'
    mongo::BSONObjIterator iter(obj);
    mmb::Element docChild = doc.root().leftChild();
    ASSERT_TRUE(docChild.ok());
    ASSERT_EQUALS(iter.next().toString(), docChild.toString());

    // 'b'
    docChild = docChild.rightSibling();
    ASSERT_TRUE(docChild.ok());
    ASSERT_TRUE(iter.more());
    ASSERT_EQUALS(iter.next().toString(), mmb::ConstElement(docChild).toString());

    // 'c'
    docChild = docChild.rightSibling();
    ASSERT_TRUE(docChild.ok());
    ASSERT_TRUE(iter.more());
    ASSERT_EQUALS(iter.next().toString(), docChild.toString());

    // eoo
    docChild = docChild.rightSibling();
    ASSERT_FALSE(iter.more());
    ASSERT_FALSE(docChild.ok());
}

TEST(DecimalType, createElement) {
    mmb::Document doc;

    mmb::Element d0 = doc.makeElementDecimal("d0", mongo::Decimal128("12345"));
    ASSERT_TRUE(mongo::Decimal128("12345").isEqual(d0.getValueDecimal()));
}

TEST(DecimalType, setElement) {
    mmb::Document doc;

    mmb::Element d0 = doc.makeElementDecimal("d0", mongo::Decimal128("128"));
    d0.setValueDecimal(mongo::Decimal128("123456")).transitional_ignore();
    ASSERT_TRUE(mongo::Decimal128("123456").isEqual(d0.getValueDecimal()));

    d0.setValueDouble(0.1).transitional_ignore();
    ASSERT_EQUALS(0.1, d0.getValueDouble());
    d0.setValueDecimal(mongo::Decimal128("23")).transitional_ignore();
    ASSERT_TRUE(mongo::Decimal128("23").isEqual(d0.getValueDecimal()));
}

TEST(DecimalType, appendElement) {
    mmb::Document doc;

    mmb::Element d0 = doc.makeElementObject("e0");
    d0.appendDecimal("precision", mongo::Decimal128(34)).transitional_ignore();

    mmb::Element it = mmb::findFirstChildNamed(d0, "precision");
    ASSERT_TRUE(it.ok());
    ASSERT_TRUE(mongo::Decimal128(34).isEqual(it.getValueDecimal()));
}

TEST(TimestampType, createElement) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementTimestamp("t0", mongo::Timestamp());
    ASSERT(mongo::Timestamp() == t0.getValueTimestamp());

    mmb::Element t1 = doc.makeElementTimestamp("t1", mongo::Timestamp(123, 456));
    ASSERT(mongo::Timestamp(123, 456) == t1.getValueTimestamp());
}

TEST(TimestampType, setElement) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementTimestamp("t0", mongo::Timestamp());
    t0.setValueTimestamp(mongo::Timestamp(123, 456)).transitional_ignore();
    ASSERT(mongo::Timestamp(123, 456) == t0.getValueTimestamp());

    // Try setting to other types and back to Timestamp
    t0.setValueLong(1234567890).transitional_ignore();
    ASSERT_EQUALS(1234567890LL, t0.getValueLong());
    t0.setValueTimestamp(mongo::Timestamp(789, 321)).transitional_ignore();
    ASSERT(mongo::Timestamp(789, 321) == t0.getValueTimestamp());

    t0.setValueString("foo bar baz").transitional_ignore();
    ASSERT_EQUALS("foo bar baz", t0.getValueString());
    t0.setValueTimestamp(mongo::Timestamp(9876, 5432)).transitional_ignore();
    ASSERT(mongo::Timestamp(9876, 5432) == t0.getValueTimestamp());
}

TEST(TimestampType, appendElement) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementObject("e0");
    t0.appendTimestamp("a timestamp field", mongo::Timestamp(1352151971, 471))
        .transitional_ignore();

    mmb::Element it = mmb::findFirstChildNamed(t0, "a timestamp field");
    ASSERT_TRUE(it.ok());
    ASSERT(mongo::Timestamp(1352151971, 471) == it.getValueTimestamp());
}

TEST(SafeNumType, createElement) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementSafeNum("t0", mongo::SafeNum(123.456));
    ASSERT_EQUALS(mongo::SafeNum(123.456), t0.getValueSafeNum());
}

// Try getting SafeNums from different types.
TEST(SafeNumType, getSafeNum) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementInt("t0", 1234567890);
    ASSERT_EQUALS(1234567890, t0.getValueInt());
    mongo::SafeNum num = t0.getValueSafeNum();
    ASSERT_EQUALS(num, static_cast<int64_t>(1234567890));

    t0.setValueLong(1234567890LL).transitional_ignore();
    ASSERT_EQUALS(1234567890LL, t0.getValueLong());
    num = t0.getValueSafeNum();
    ASSERT_EQUALS(num, static_cast<int64_t>(1234567890LL));

    t0.setValueDouble(123.456789).transitional_ignore();
    ASSERT_EQUALS(123.456789, t0.getValueDouble());
    num = t0.getValueSafeNum();
    ASSERT_EQUALS(num, 123.456789);

    t0.setValueDecimal(mongo::Decimal128("12345678.1234")).transitional_ignore();
    ASSERT_TRUE(mongo::Decimal128("12345678.1234").isEqual(t0.getValueDecimal()));
    num = t0.getValueSafeNum();
    ASSERT_EQUALS(num, mongo::Decimal128("12345678.1234"));
}

TEST(SafeNumType, setSafeNum) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementSafeNum("t0", mongo::SafeNum(123456));
    t0.setValueSafeNum(mongo::SafeNum(654321)).transitional_ignore();
    ASSERT_EQUALS(mongo::SafeNum(654321), t0.getValueSafeNum());

    // Try setting to other types and back to SafeNum
    t0.setValueLong(1234567890).transitional_ignore();
    ASSERT_EQUALS(1234567890LL, t0.getValueLong());
    t0.setValueSafeNum(mongo::SafeNum(1234567890)).transitional_ignore();
    ASSERT_EQUALS(mongo::SafeNum(1234567890), t0.getValueSafeNum());

    t0.setValueString("foo bar baz").transitional_ignore();

    mongo::StringData left = "foo bar baz";
    mongo::StringData right = t0.getValueString();
    ASSERT_EQUALS(left, right);

    ASSERT_EQUALS(mongo::StringData("foo bar baz"), t0.getValueString());
    t0.setValueSafeNum(mongo::SafeNum(12345)).transitional_ignore();
    ASSERT_EQUALS(mongo::SafeNum(12345), t0.getValueSafeNum());
}

TEST(SafeNumType, appendElement) {
    mmb::Document doc;

    mmb::Element t0 = doc.makeElementObject("e0");
    t0.appendSafeNum("a timestamp field", mongo::SafeNum(static_cast<int64_t>(1352151971LL)))
        .transitional_ignore();

    mmb::Element it = findFirstChildNamed(t0, "a timestamp field");
    ASSERT_TRUE(it.ok());
    ASSERT_EQUALS(mongo::SafeNum(static_cast<int64_t>(1352151971LL)), it.getValueSafeNum());
}

TEST(OIDType, getOidValue) {
    mmb::Document doc;
    mmb::Element t0 = doc.makeElementObject("e0");
    const mongo::OID generated = mongo::OID::gen();
    t0.appendOID("myOid", generated).transitional_ignore();
    mmb::Element it = findFirstChildNamed(t0, "myOid");
    const mongo::OID recovered = mongo::OID(it.getValueOID());
    ASSERT_EQUALS(generated, recovered);
}

TEST(OIDType, nullOID) {
    mmb::Document doc;
    mmb::Element t0 = doc.makeElementObject("e0");
    const mongo::OID withNull("50a9c82263e413ad0028faad");
    t0.appendOID("myOid", withNull).transitional_ignore();
    mmb::Element it = findFirstChildNamed(t0, "myOid");
    const mongo::OID recovered = mongo::OID(it.getValueOID());
    ASSERT_EQUALS(withNull, recovered);
}

static const char jsonSample[] =
    "{_id:ObjectId(\"47cc67093475061e3d95369d\"),"
    "query:\"kate hudson\","
    "owner:1234567887654321,"
    "date:\"2011-05-13T14:22:46.777Z\","
    "score:123.456,"
    "field1:Infinity,"
    "\"field2\":-Infinity,"
    "\"field3\":NaN,"
    "users:["
    "{uname:\"@aaaa\",editid:\"123\",date:1303959350,yes_votes:0,no_votes:0},"
    "{uname:\"@bbbb\",editid:\"456\",date:1303959350,yes_votes:0,no_votes:0},"
    "{uname:\"@cccc\",editid:\"789\",date:1303959350,yes_votes:0,no_votes:0}],"
    "pattern:/match.*this/,"
    "lastfield:\"last\"}";

static const char jsonSampleWithDecimal[] =
    "{_id:ObjectId(\"47cc67093475061e3d95369d\"),"
    "query:\"kate hudson\","
    "owner:1234567887654321,"
    "date:\"2011-05-13T14:22:46.777Z\","
    "score:123.456,"
    "decimal:NumberDecimal(\"2\"),"
    "field1:Infinity,"
    "\"field2\":-Infinity,"
    "\"field3\":NaN,"
    "users:["
    "{uname:\"@aaaa\",editid:\"123\",date:1303959350,yes_votes:0,no_votes:0},"
    "{uname:\"@bbbb\",editid:\"456\",date:1303959350,yes_votes:0,no_votes:0},"
    "{uname:\"@cccc\",editid:\"789\",date:1303959350,yes_votes:0,no_votes:0}],"
    "pattern:/match.*this/,"
    "lastfield:\"last\"}";

TEST(Serialization, RoundTrip) {
    mongo::BSONObj obj;
    obj = mongo::fromjson(jsonSampleWithDecimal);
    mmb::Document doc(obj.copy());
    mongo::BSONObj built = doc.getObject();
    ASSERT_BSONOBJ_EQ(obj, built);
}

TEST(Documentation, Example1) {
    // Create a new document
    mmb::Document doc;
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{}"), doc.getObject());

    // Get the root of the document.
    mmb::Element root = doc.root();

    // Create a new mongo::BSONType::numberInt typed Element to represent life, the universe, and
    // everything, then push that Element into the root object, making it a child of root.
    mmb::Element e0 = doc.makeElementInt("ltuae", 42);
    ASSERT_OK(root.pushBack(e0));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ ltuae : 42 }"), doc.getObject());

    // Create a new empty mongo::Object-typed Element named 'magic', and push it back as a
    // child of the root, making it a sibling of e0.
    mmb::Element e1 = doc.makeElementObject("magic");
    ASSERT_OK(root.pushBack(e1));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ ltuae : 42, magic : {} }"), doc.getObject());

    // Create a new mongo::BSONType::numberDouble typed Element to represent Pi, and insert it as
    // child of the new object we just created.
    mmb::Element e3 = doc.makeElementDouble("pi", 3.14);
    ASSERT_OK(e1.pushBack(e3));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ ltuae : 42, magic : { pi : 3.14 } }"), doc.getObject());

    // Create a new mongo::BSONType::numberDouble to represent Plancks constant in electrovolt
    // micrometers, and add it as a child of the 'magic' object.
    mmb::Element e4 = doc.makeElementDouble("hbar", 1.239);
    ASSERT_OK(e1.pushBack(e4));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ ltuae : 42, magic : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

    // Rename the parent element of 'hbar' to be 'constants'.
    ASSERT_OK(e4.parent().rename("constants"));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ ltuae : 42, constants : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

    // Rename 'ltuae' to 'answer' by accessing it as the root objects left child.
    ASSERT_OK(doc.root().leftChild().rename("answer"));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ answer : 42, constants : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

    // Sort the constants by name.
    mmb::sortChildren(doc.root().rightChild(), mmb::FieldNameLessThan());
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ answer : 42, constants : { hbar : 1.239, pi : 3.14 } }"),
                      doc.getObject());
}

TEST(Documentation, Example2) {
    static const char inJson[] =
        "{"
        "  'whale': { 'alive': true, 'dv': -9.8, 'height': 50.0, attrs : [ 'big' ] },"
        "  'petunias': { 'alive': true, 'dv': -9.8, 'height': 50.0 } "
        "}";
    mongo::BSONObj obj = mongo::fromjson(inJson);

    // Create a new document representing BSONObj with the above contents.
    mmb::Document doc(obj);

    // The whale hits the planet and dies.
    mmb::Element whale = mmb::findFirstChildNamed(doc.root(), "whale");
    ASSERT_TRUE(whale.ok());
    // Find the 'dv' field in the whale.
    mmb::Element whale_deltav = mmb::findFirstChildNamed(whale, "dv");
    ASSERT_TRUE(whale_deltav.ok());
    // Set the dv field to zero.
    ASSERT_OK(whale_deltav.setValueDouble(0.0));
    // Find the 'height' field in the whale.
    mmb::Element whale_height = mmb::findFirstChildNamed(whale, "height");
    ASSERT_TRUE(whale_height.ok());
    // Set the height field to zero.
    ASSERT_OK(whale_height.setValueDouble(0));
    // Find the 'alive' field, and set it to false.
    mmb::Element whale_alive = mmb::findFirstChildNamed(whale, "alive");
    ASSERT_TRUE(whale_alive.ok());
    ASSERT_OK(whale_alive.setValueBool(false));

    // The petunias survive, update its fields much like we did above.
    mmb::Element petunias = mmb::findFirstChildNamed(doc.root(), "petunias");
    ASSERT_TRUE(petunias.ok());
    mmb::Element petunias_deltav = mmb::findFirstChildNamed(petunias, "dv");
    ASSERT_TRUE(petunias_deltav.ok());
    ASSERT_OK(petunias_deltav.setValueDouble(0.0));
    mmb::Element petunias_height = mmb::findFirstChildNamed(petunias, "height");
    ASSERT_TRUE(petunias_height.ok());
    ASSERT_OK(petunias_height.setValueDouble(0));

    // Replace the whale by its wreckage, saving only its attributes:
    // Construct a new mongo::Object element for the ex-whale.
    mmb::Element ex_whale = doc.makeElementObject("ex-whale");
    ASSERT_OK(doc.root().pushBack(ex_whale));
    // Find the attributes of the old 'whale' element.
    mmb::Element whale_attrs = mmb::findFirstChildNamed(whale, "attrs");
    // Remove the attributes from the whale (they remain valid, but detached).
    ASSERT_OK(whale_attrs.remove());
    // Insert the attributes into the ex-whale.
    ASSERT_OK(ex_whale.pushBack(whale_attrs));
    // Remove the whale object.
    ASSERT_OK(whale.remove());

    static const char outJson[] =
        "{"
        "    'petunias': { 'alive': true, 'dv': 0.0, 'height': 0 },"
        "    'ex-whale': { 'attrs': [ 'big' ] }"
        "}";

    mongo::BSONObjBuilder builder;
    doc.writeTo(&builder);
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), doc.getObject());
}

namespace {
void apply(mongo::BSONObj* obj, const DamageVector& damages, const char* source) {
    const DamageVector::const_iterator end = damages.end();
    DamageVector::const_iterator where = damages.begin();
    char* const target = const_cast<char*>(obj->objdata());
    for (; where != end; ++where) {
        std::memcpy(target + where->targetOffset, source + where->sourceOffset, where->sourceSize);
    }
}
}  // namespace

TEST(Documentation, Example2InPlaceWithDamageVector) {
    static const char inJson[] =
        "{"
        "  'whale': { 'alive': true, 'dv': -9.8, 'height': 50.0, attrs : [ 'big' ] },"
        "  'petunias': { 'alive': true, 'dv': -9.8, 'height': 50.0 } "
        "}";

    // Make the object, and make a copy for reference.
    mongo::BSONObj obj = mongo::fromjson(inJson);
    const mongo::BSONObj copyOfObj = obj.getOwned();
    ASSERT_BSONOBJ_EQ(obj, copyOfObj);

    // Create a new document representing BSONObj with the above contents.
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_EQUALS(obj, doc);
    ASSERT_EQUALS(copyOfObj, doc);

    // Enable in-place mutation for this document
    ASSERT_EQUALS(mmb::Document::kInPlaceEnabled, doc.getCurrentInPlaceMode());

    // The whale hits the planet and dies.
    mmb::Element whale = mmb::findFirstChildNamed(doc.root(), "whale");
    ASSERT_TRUE(whale.ok());
    // Find the 'dv' field in the whale.
    mmb::Element whale_deltav = mmb::findFirstChildNamed(whale, "dv");
    ASSERT_TRUE(whale_deltav.ok());
    // Set the dv field to zero.
    ASSERT_OK(whale_deltav.setValueDouble(0.0));
    // Find the 'height' field in the whale.
    mmb::Element whale_height = mmb::findFirstChildNamed(whale, "height");
    ASSERT_TRUE(whale_height.ok());
    // Set the height field to zero.
    ASSERT_OK(whale_height.setValueDouble(0));
    // Find the 'alive' field, and set it to false.
    mmb::Element whale_alive = mmb::findFirstChildNamed(whale, "alive");
    ASSERT_TRUE(whale_alive.ok());
    ASSERT_OK(whale_alive.setValueBool(false));

    // The petunias survive, update its fields much like we did above.
    mmb::Element petunias = mmb::findFirstChildNamed(doc.root(), "petunias");
    ASSERT_TRUE(petunias.ok());
    mmb::Element petunias_deltav = mmb::findFirstChildNamed(petunias, "dv");
    ASSERT_TRUE(petunias_deltav.ok());
    ASSERT_OK(petunias_deltav.setValueDouble(0.0));
    mmb::Element petunias_height = mmb::findFirstChildNamed(petunias, "height");
    ASSERT_TRUE(petunias_height.ok());
    ASSERT_OK(petunias_height.setValueDouble(0));

    // Demonstrate that while the document has changed, the underlying BSONObj has not yet
    // changed.
    ASSERT_FALSE(obj == doc);
    ASSERT_BSONOBJ_EQ(copyOfObj, obj);

    // Ensure that in-place updates are still enabled.
    ASSERT_EQUALS(mmb::Document::kInPlaceEnabled, doc.getCurrentInPlaceMode());

    // Extract the damage events
    DamageVector damages;
    const char* source = nullptr;
    size_t size = 0;
    ASSERT_EQUALS(true, doc.getInPlaceUpdates(&damages, &source, &size));
    ASSERT_NOT_EQUALS(0U, damages.size());
    ASSERT_NOT_EQUALS(static_cast<const char*>(nullptr), source);
    ASSERT_NOT_EQUALS(0U, size);

    apply(&obj, damages, source);

    static const char outJson[] =
        "{"
        "  'whale': { 'alive': false, 'dv': 0, 'height': 0, attrs : [ 'big' ] },"
        "  'petunias': { 'alive': true, 'dv': 0, 'height': 0 } "
        "}";
    mongo::BSONObj outObj = mongo::fromjson(outJson);

    ASSERT_EQUALS(outObj, doc);

    mongo::BSONObjBuilder builder;
    doc.writeTo(&builder);
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), doc.getObject());
}

TEST(Documentation, Example3) {
    static const char inJson[] =
        "{"
        "  'xs': { 'x' : 'x', 'X' : 'X' },"
        "  'ys': { 'y' : 'y' }"
        "}";
    mongo::BSONObj inObj = mongo::fromjson(inJson);

    mmb::Document doc(inObj);
    mmb::Element xs = doc.root().leftChild();
    ASSERT_TRUE(xs.ok());
    mmb::Element ys = xs.rightSibling();
    ASSERT_TRUE(ys.ok());
    mmb::Element dne = ys.rightSibling();
    ASSERT_FALSE(dne.ok());
    mmb::Element ycaps = doc.makeElementString("Y", "Y");
    ASSERT_OK(ys.pushBack(ycaps));
    mmb::Element pun = doc.makeElementArray("why");
    ASSERT_OK(ys.pushBack(pun));
    pun.appendString("na", "not").transitional_ignore();
    mongo::BSONObj outObj = doc.getObject();

    static const char outJson[] =
        "{"
        "  'xs': { 'x' : 'x', 'X' : 'X' },"
        "  'ys': { 'y' : 'y', 'Y' : 'Y', 'why' : ['not'] }"
        "}";
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), outObj);
}

TEST(Document, LifecycleConstructDefault) {
    // Verify the state of a newly created empty Document.
    mmb::Document doc;
    ASSERT_TRUE(doc.root().ok());
    ASSERT_TRUE(const_cast<const mmb::Document&>(doc).root().ok());
    ASSERT_TRUE(doc.root().isType(BSONType::object));
    ASSERT_FALSE(doc.root().leftSibling().ok());
    ASSERT_FALSE(doc.root().rightSibling().ok());
    ASSERT_FALSE(doc.root().leftChild().ok());
    ASSERT_FALSE(doc.root().rightChild().ok());
    ASSERT_FALSE(doc.root().parent().ok());
    ASSERT_FALSE(doc.root().hasValue());
}

TEST(Document, LifecycleConstructEmptyBSONObj) {
    // Verify the state of a newly created empty Document where the construction argument
    // is an empty BSONObj.
    mongo::BSONObj obj;
    mmb::Document doc(obj);
    ASSERT_TRUE(doc.root().ok());
    ASSERT_TRUE(const_cast<const mmb::Document&>(doc).root().ok());
    ASSERT_TRUE(doc.root().isType(BSONType::object));
    ASSERT_FALSE(doc.root().leftSibling().ok());
    ASSERT_FALSE(doc.root().rightSibling().ok());
    ASSERT_FALSE(doc.root().leftChild().ok());
    ASSERT_FALSE(doc.root().rightChild().ok());
    ASSERT_FALSE(doc.root().parent().ok());
    ASSERT_FALSE(doc.root().hasValue());
}

TEST(Document, LifecycleConstructSimpleBSONObj) {
    // Verify the state of a newly created Document where the construction argument is a
    // simple (flat) BSONObj.
    mongo::BSONObj obj = mongo::fromjson("{ e1: 1, e2: 'hello', e3: false }");
    mmb::Document doc(obj);

    // Check the state of the root.
    ASSERT_TRUE(doc.root().ok());
    ASSERT_TRUE(const_cast<const mmb::Document&>(doc).root().ok());
    ASSERT_TRUE(doc.root().isType(BSONType::object));
    ASSERT_FALSE(doc.root().parent().ok());
    ASSERT_FALSE(doc.root().leftSibling().ok());
    ASSERT_FALSE(doc.root().rightSibling().ok());
    ASSERT_FALSE(doc.root().hasValue());

    mmb::ConstElement e1Child = doc.root().leftChild();
    // Check the connectivity of 'e1'.
    ASSERT_TRUE(e1Child.ok());
    ASSERT_EQUALS(doc.root(), e1Child.parent());
    ASSERT_FALSE(e1Child.leftSibling().ok());
    ASSERT_TRUE(e1Child.rightSibling().ok());
    ASSERT_FALSE(e1Child.leftChild().ok());
    ASSERT_FALSE(e1Child.rightChild().ok());

    // Check the type, name, and value of 'e1'.
    ASSERT_TRUE(e1Child.isType(BSONType::numberInt));
    ASSERT_EQUALS("e1", e1Child.getFieldName());
    ASSERT_TRUE(e1Child.hasValue());
    ASSERT_EQUALS(int32_t(1), e1Child.getValueInt());

    mmb::ConstElement e2Child = e1Child.rightSibling();
    // Check the connectivity of 'e2'.
    ASSERT_TRUE(e2Child.ok());
    ASSERT_EQUALS(doc.root(), e2Child.parent());
    ASSERT_TRUE(e2Child.leftSibling().ok());
    ASSERT_TRUE(e2Child.rightSibling().ok());
    ASSERT_EQUALS(e1Child, e2Child.leftSibling());
    ASSERT_FALSE(e2Child.leftChild().ok());
    ASSERT_FALSE(e2Child.rightChild().ok());

    // Check the type, name and value of 'e2'.
    ASSERT_TRUE(e2Child.isType(BSONType::string));
    ASSERT_EQUALS("e2", e2Child.getFieldName());
    ASSERT_TRUE(e2Child.hasValue());
    ASSERT_EQUALS("hello", e2Child.getValueString());

    mmb::ConstElement e3Child = e2Child.rightSibling();
    // Check the connectivity of 'e3'.
    ASSERT_TRUE(e3Child.ok());
    ASSERT_EQUALS(doc.root(), e3Child.parent());
    ASSERT_TRUE(e3Child.leftSibling().ok());
    ASSERT_FALSE(e3Child.rightSibling().ok());
    ASSERT_EQUALS(e2Child, e3Child.leftSibling());
    ASSERT_FALSE(e2Child.leftChild().ok());
    ASSERT_FALSE(e2Child.rightChild().ok());

    // Check the type, name and value of 'e3'.
    ASSERT_TRUE(e3Child.isType(BSONType::boolean));
    ASSERT_EQUALS("e3", e3Child.getFieldName());
    ASSERT_TRUE(e3Child.hasValue());
    ASSERT_EQUALS(false, e3Child.getValueBool());
}

TEST(Document, RenameDeserialization) {
    // Regression test for a bug where certain rename operations failed to deserialize up
    // the tree correctly, resulting in a lost rename
    static const char inJson[] =
        "{"
        "  'a' : { 'b' : { 'c' : { 'd' : 4 } } }"
        "}";
    mongo::BSONObj inObj = mongo::fromjson(inJson);

    mmb::Document doc(inObj);
    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    mmb::Element b = a.leftChild();
    ASSERT_TRUE(b.ok());
    mmb::Element c = b.leftChild();
    ASSERT_TRUE(c.ok());
    c.rename("C").transitional_ignore();
    mongo::BSONObj outObj = doc.getObject();
    static const char outJson[] =
        "{"
        "  'a' : { 'b' : { 'C' : { 'd' : 4 } } }"
        "}";
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), outObj);
}

TEST(Document, CantRenameRootElement) {
    mmb::Document doc;
    ASSERT_NOT_OK(doc.root().rename("foo"));
}

TEST(Document, RemoveElementWithOpaqueRightSibling) {
    // Regression test for a bug where removing an element with an opaque right sibling
    // would access an invalidated rep. Note that this test may or may not fail depending
    // on the details of memory allocation: failures would be clearly visible with
    // valgrind, however.
    static const char inJson[] =
        "{"
        "  'a' : 1, 'b' : 2, 'c' : 3"
        "}";

    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    a.remove().transitional_ignore();

    static const char outJson[] =
        "{"
        "  'b' : 2, 'c' : 3"
        "}";
    mongo::BSONObj outObj = doc.getObject();
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), outObj);
}

TEST(Document, AddRightSiblingToElementWithOpaqueRightSibling) {
    // Regression test for a bug where adding a right sibling to a node with an opaque
    // right sibling would potentially access an invalidated rep. Like the 'remove' test
    // above, this may or may not crash, but would be visible under a memory checking tool.
    static const char inJson[] =
        "{"
        "  'a' : 1, 'b' : 2, 'c' : 3"
        "}";

    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    mmb::Element newElt = doc.makeElementString("X", "X");
    ASSERT_OK(a.addSiblingRight(newElt));

    static const char outJson[] =
        "{"
        "  'a' : 1, 'X' : 'X', 'b' : 2, 'c' : 3"
        "}";
    mongo::BSONObj outObj = doc.getObject();
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), outObj);
}

TEST(Document, ArrayIndexedAccessFromJson) {
    static const char inJson[] =
        "{"
        " a : 1, b : [{ c : 1 }]"
        "}";

    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS("a", a.getFieldName());
    ASSERT_EQUALS(BSONType::numberInt, a.getType());

    mmb::Element b = a.rightSibling();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS("b", b.getFieldName());
    ASSERT_EQUALS(BSONType::array, b.getType());

    mmb::Element b0 = b[0];
    ASSERT_TRUE(b0.ok());
    ASSERT_EQUALS("0", b0.getFieldName());
    ASSERT_EQUALS(BSONType::object, b0.getType());
}

TEST(Document, ArrayIndexedAccessFromManuallyBuilt) {
    mmb::Document doc;
    mmb::Element root = doc.root();
    ASSERT_TRUE(root.ok());
    {
        ASSERT_OK(root.appendInt("a", 1));
        mmb::Element b = doc.makeElementArray("b");
        ASSERT_TRUE(b.ok());
        ASSERT_OK(root.pushBack(b));
        mmb::Element b0 = doc.makeElementObject("ignored");
        ASSERT_TRUE(b0.ok());
        ASSERT_OK(b.pushBack(b0));
        ASSERT_OK(b0.appendInt("c", 1));
    }

    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS("a", a.getFieldName());
    ASSERT_EQUALS(BSONType::numberInt, a.getType());

    mmb::Element b = a.rightSibling();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS("b", b.getFieldName());
    ASSERT_EQUALS(BSONType::array, b.getType());

    mmb::Element b0 = b[0];
    ASSERT_TRUE(b0.ok());
    ASSERT_EQUALS("ignored", b0.getFieldName());
    ASSERT_EQUALS(BSONType::object, b0.getType());
}

TEST(Document, EndElement) {
    mmb::Document doc;
    mmb::Element end = doc.end();
    ASSERT_FALSE(end.ok());
    mmb::Element missing = doc.root().leftChild();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().rightChild();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().leftSibling();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().rightSibling();
    ASSERT_EQUALS(end, missing);
}

TEST(Document, ConstEndElement) {
    const mmb::Document doc;
    mmb::ConstElement end = doc.end();
    ASSERT_FALSE(end.ok());
    mmb::ConstElement missing = doc.root().leftChild();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().rightChild();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().leftSibling();
    ASSERT_EQUALS(end, missing);
    missing = doc.root().rightSibling();
    ASSERT_EQUALS(end, missing);
}

TEST(Element, EmptyDocHasNoChildren) {
    mmb::Document doc;
    ASSERT_FALSE(doc.root().hasChildren());
}

TEST(Element, PopulatedDocHasChildren) {
    mmb::Document doc;
    ASSERT_OK(doc.root().appendInt("a", 1));
    ASSERT_TRUE(doc.root().hasChildren());
    mmb::Element lc = doc.root().leftChild();
    ASSERT_FALSE(lc.hasChildren());
}

TEST(Element, LazyEmptyDocHasNoChildren) {
    static const char inJson[] = "{}";
    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);
    ASSERT_FALSE(doc.root().hasChildren());
}

TEST(Element, LazySingletonDocHasChildren) {
    static const char inJson[] = "{ a : 1 }";
    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);
    ASSERT_TRUE(doc.root().hasChildren());
    ASSERT_FALSE(doc.root().leftChild().hasChildren());
}

TEST(Element, LazyConstDoubletonDocHasChildren) {
    static const char inJson[] = "{ a : 1, b : 2 }";
    mongo::BSONObj inObj = mongo::fromjson(inJson);
    const mmb::Document doc(inObj);
    ASSERT_TRUE(doc.root().hasChildren());
    ASSERT_FALSE(doc.root().leftChild().hasChildren());
    ASSERT_FALSE(doc.root().rightChild().hasChildren());
    ASSERT_FALSE(doc.root().leftChild() == doc.root().rightChild());
}

TEST(Document, AddChildToEmptyOpaqueSubobject) {
    mongo::BSONObj inObj = mongo::fromjson("{a: {}}");
    mmb::Document doc(inObj);

    mmb::Element elem = doc.root()["a"];
    ASSERT_TRUE(elem.ok());

    mmb::Element newElem = doc.makeElementInt("0", 1);
    ASSERT_TRUE(newElem.ok());

    ASSERT_OK(elem.pushBack(newElem));
}

TEST(Element, IsNumeric) {
    mmb::Document doc;

    mmb::Element elt = doc.makeElementNull("dummy");
    ASSERT_FALSE(elt.isNumeric());

    elt = doc.makeElementInt("dummy", 42);
    ASSERT_TRUE(elt.isNumeric());

    elt = doc.makeElementString("dummy", "dummy");
    ASSERT_FALSE(elt.isNumeric());

    elt = doc.makeElementLong("dummy", 42);
    ASSERT_TRUE(elt.isNumeric());

    elt = doc.makeElementBool("dummy", false);
    ASSERT_FALSE(elt.isNumeric());

    elt = doc.makeElementDouble("dummy", 42.0);
    ASSERT_TRUE(elt.isNumeric());

    elt = doc.makeElementDecimal("dummy", mongo::Decimal128(20));
    ASSERT_TRUE(elt.isNumeric());
}

TEST(Element, IsIntegral) {
    mmb::Document doc;

    mmb::Element elt = doc.makeElementNull("dummy");
    ASSERT_FALSE(elt.isIntegral());

    elt = doc.makeElementInt("dummy", 42);
    ASSERT_TRUE(elt.isIntegral());

    elt = doc.makeElementString("dummy", "dummy");
    ASSERT_FALSE(elt.isIntegral());

    elt = doc.makeElementLong("dummy", 42);
    ASSERT_TRUE(elt.isIntegral());

    elt = doc.makeElementDouble("dummy", 42.0);
    ASSERT_FALSE(elt.isIntegral());

    elt = doc.makeElementDecimal("dummy", mongo::Decimal128(20));
    ASSERT_FALSE(elt.isIntegral());
}

TEST(Document, ArraySerialization) {
    static const char inJson[] =
        "{ "
        " 'a' : { 'b' : [ 'c', 'd' ] } "
        "}";

    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

    mmb::Element root = doc.root();
    mmb::Element a = root.leftChild();
    mmb::Element b = a.leftChild();
    mmb::Element new_array = doc.makeElementArray("XXX");
    mmb::Element e = doc.makeElementString("e", "e");
    new_array.pushBack(e).transitional_ignore();
    b.pushBack(new_array).transitional_ignore();

    static const char outJson[] =
        "{ "
        " 'a' : { 'b' : [ 'c', 'd', [ 'e' ] ] } "
        "}";

    const mongo::BSONObj outObj = doc.getObject();
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), outObj);
}

TEST(Document, SetValueBSONElementFieldNameHandling) {
    static const char inJson[] = "{ a : 4 }";
    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

    static const char inJson2[] = "{ b : 5 }";
    mongo::BSONObj inObj2 = mongo::fromjson(inJson2);
    mongo::BSONObjIterator iterator(inObj2);

    ASSERT_TRUE(iterator.more());
    const mongo::BSONElement b = iterator.next();

    mmb::Element a = doc.root().leftChild();
    a.setValueBSONElement(b).transitional_ignore();

    static const char outJson[] = "{ a : 5 }";
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), doc.getObject());
}

TEST(Document, SetValueElementFromSeparateDocument) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ b : 5 }");
    const mmb::Document doc2(inObj2);

    auto setTo = doc1.root().leftChild();
    auto setFrom = doc2.root().leftChild();
    ASSERT_OK(setTo.setValueElement(setFrom));

    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ a : 5 }"), doc1.getObject());

    // Doc containing the 'setFrom' element should be unchanged.
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, SetValueElementIsNoopWhenSetToSelf) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc(inObj);

    auto element = doc.root().leftChild();
    ASSERT_OK(element.setValueElement(element));

    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementIsNoopWhenSetToSelfFromCopy) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc(inObj);

    auto element = doc.root().leftChild();
    auto elementCopy = element;
    ASSERT_OK(element.setValueElement(elementCopy));

    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementIsNoopWhenSetToSelfNonRootElement) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : { b : { c: 4 } } }");
    mmb::Document doc(inObj);

    auto element = doc.root().leftChild().leftChild().leftChild();
    ASSERT_EQ("c", element.getFieldName());
    ASSERT_OK(element.setValueElement(element));

    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementSetToNestedObject) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ b : { c : 5, d : 6 } }");
    const mmb::Document doc2(inObj2);

    auto setTo = doc1.root().leftChild();
    auto setFrom = doc2.root().leftChild();
    ASSERT_OK(setTo.setValueElement(setFrom));

    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ a : { c : 5, d : 6 } }"), doc1.getObject());

    // Doc containing the 'setFrom' element should be unchanged.
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, SetValueElementNonRootElements) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : { b : 5, c : 6 } }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ d : { e : 8, f : 9 } }");
    const mmb::Document doc2(inObj2);

    auto setTo = doc1.root().leftChild().rightChild();
    ASSERT_EQ("c", setTo.getFieldName());
    auto setFrom = doc2.root().leftChild().leftChild();
    ASSERT_EQ("e", setFrom.getFieldName());
    ASSERT_OK(setTo.setValueElement(setFrom));

    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ a : { b : 5, c : 8 } }"), doc1.getObject());

    // Doc containing the 'setFrom' element should be unchanged.
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, SetValueElementSetRootToSelfErrors) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc(inObj);

    auto element = doc.root();
    ASSERT_NOT_OK(element.setValueElement(element));
    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementSetRootToAnotherDocRootErrors) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ b : 5 }");
    const mmb::Document doc2(inObj2);

    auto setTo = doc1.root();
    auto setFrom = doc2.root();
    ASSERT_NOT_OK(setTo.setValueElement(setFrom));

    ASSERT_BSONOBJ_EQ(inObj, doc1.getObject());
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, SetValueElementSetRootToNotRootInSelfErrors) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc(inObj);

    auto setTo = doc.root();
    auto setFrom = doc.root().leftChild();
    ASSERT_NOT_OK(setTo.setValueElement(setFrom));
    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementSetRootToNotRootInAnotherDocErrors) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : 4 }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ b : 5 }");
    const mmb::Document doc2(inObj2);

    auto setTo = doc1.root();
    auto setFrom = doc2.root().leftChild();
    ASSERT_NOT_OK(setTo.setValueElement(setFrom));

    ASSERT_BSONOBJ_EQ(inObj, doc1.getObject());
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, SetValueElementSetToOwnRootErrors) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : { b : 4 } }");
    mmb::Document doc(inObj);

    auto setTo = doc.root().leftChild().leftChild();
    ASSERT_EQ("b", setTo.getFieldName());
    auto setFrom = doc.root();

    ASSERT_NOT_OK(setTo.setValueElement(setFrom));
    ASSERT_BSONOBJ_EQ(inObj, doc.getObject());
}

TEST(Document, SetValueElementSetToOtherDocRoot) {
    mongo::BSONObj inObj = mongo::fromjson("{ a : { b : 4 } }");
    mmb::Document doc1(inObj);

    mongo::BSONObj inObj2 = mongo::fromjson("{ c : 5 }");
    mmb::Document doc2(inObj2);

    auto setTo = doc1.root().leftChild().leftChild();
    ASSERT_EQ("b", setTo.getFieldName());
    auto setFrom = doc2.root();

    ASSERT_OK(setTo.setValueElement(setFrom));
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{ a : { b : { c : 5 } } }"), doc1.getObject());
    ASSERT_BSONOBJ_EQ(inObj2, doc2.getObject());
}

TEST(Document, CreateElementWithEmptyFieldName) {
    mmb::Document doc;
    mmb::Element noname = doc.makeElementObject(mongo::StringData());
    ASSERT_TRUE(noname.ok());
    ASSERT_EQUALS(mongo::StringData(), noname.getFieldName());
}

TEST(Document, CreateElementFromBSONElement) {
    mongo::BSONObj obj = mongo::fromjson("{a:1}");
    mmb::Document doc;
    ASSERT_OK(doc.root().appendElement(obj["a"]));

    mmb::Element newElem = doc.root()["a"];
    ASSERT_TRUE(newElem.ok());
    ASSERT_EQUALS(newElem.getType(), mongo::BSONType::numberInt);
    ASSERT_EQUALS(newElem.getValueInt(), 1);
}

TEST(Document, toStringEmpty) {
    mongo::BSONObj obj;
    mmb::Document doc;
    ASSERT_EQUALS(obj.toString(), doc.toString());
}

TEST(Document, toStringComplex) {
    mongo::BSONObj obj = mongo::fromjson("{a : 1, b : [1, 2, 3], c : 'c'}");
    mmb::Document doc(obj);
    ASSERT_EQUALS(obj.toString(), doc.toString());
}

TEST(Document, toStringEphemeralObject) {
    mmb::Document doc;
    mmb::Element e = doc.makeElementObject("foo");
    ASSERT_OK(doc.root().pushBack(e));
    ASSERT_OK(e.appendDouble("d", 1.0));
    ASSERT_OK(e.appendString("s", "str"));
    ASSERT_EQUALS(mongo::fromjson("{ foo: { d : 1.0, s : 'str' } }").firstElement().toString(),
                  e.toString());
}

TEST(Document, toStringEphemeralArray) {
    mmb::Document doc;
    mmb::Element e = doc.makeElementArray("foo");
    ASSERT_OK(doc.root().pushBack(e));
    ASSERT_OK(e.appendDouble(mongo::StringData(), 1.0));
    ASSERT_OK(e.appendString(mongo::StringData(), "str"));
    ASSERT_EQUALS(mongo::fromjson("{ foo: [ 1.0, 'str' ] }").firstElement().toString(),
                  e.toString());
}

TEST(Document, ElementCloningToDifferentDocument) {
    const char initial[] = "{ a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6 ] }";

    mmb::Document source(mongo::fromjson(initial));

    // Dirty the 'd' node and parents.
    source.root()["d"]
        .pushBack(source.makeElementInt(mongo::StringData(), 7))
        .transitional_ignore();

    mmb::Document target;

    mmb::Element newElement = target.makeElement(source.root()["d"]);
    ASSERT_TRUE(newElement.ok());
    mongo::Status status = target.root().pushBack(newElement);
    ASSERT_OK(status);
    const char* expected = "{ d : [ 4, 5, 6, 7 ] }";
    ASSERT_EQUALS(mongo::fromjson(expected), target);

    newElement = target.makeElement(source.root()["b"]);
    ASSERT_TRUE(newElement.ok());
    status = target.root().pushBack(newElement);
    ASSERT_OK(status);
    expected = "{ d : [ 4, 5, 6, 7 ], b : [ 1, 2, 3 ] }";
    ASSERT_EQUALS(mongo::fromjson(expected), target);

    newElement = target.makeElementWithNewFieldName("C", source.root()["c"]);
    ASSERT_TRUE(newElement.ok());
    status = target.root().pushBack(newElement);
    ASSERT_OK(status);
    expected = "{ d : [ 4, 5, 6, 7 ], b : [ 1, 2, 3 ], C : { 'c' : 'c' } }";
    ASSERT_EQUALS(mongo::fromjson(expected), target);
}

TEST(Document, ElementCloningToSameDocument) {
    const char initial[] = "{ a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6 ] }";

    mmb::Document doc(mongo::fromjson(initial));

    // Dirty the 'd' node and parents.
    doc.root()["d"].pushBack(doc.makeElementInt(mongo::StringData(), 7)).transitional_ignore();

    mmb::Element newElement = doc.makeElement(doc.root()["d"]);
    ASSERT_TRUE(newElement.ok());
    mongo::Status status = doc.root().pushBack(newElement);
    ASSERT_OK(status);
    const char* expected =
        "{ "
        " a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ], "
        " d : [ 4, 5, 6, 7 ] "
        "}";
    ASSERT_EQUALS(mongo::fromjson(expected), doc);

    newElement = doc.makeElement(doc.root()["b"]);
    ASSERT_TRUE(newElement.ok());
    status = doc.root().pushBack(newElement);
    ASSERT_OK(status);
    expected =
        "{ "
        " a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ], "
        " d : [ 4, 5, 6, 7 ], "
        " b : [ 1, 2, 3 ] "
        "}";
    ASSERT_EQUALS(mongo::fromjson(expected), doc);

    newElement = doc.makeElementWithNewFieldName("C", doc.root()["c"]);
    ASSERT_TRUE(newElement.ok());
    status = doc.root().pushBack(newElement);
    ASSERT_OK(status);
    expected =
        "{ "
        " a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ], "
        " d : [ 4, 5, 6, 7 ], "
        " b : [ 1, 2, 3 ], "
        " C : { 'c' : 'c' } "
        "}";
    ASSERT_EQUALS(mongo::fromjson(expected), doc);
}

TEST(Document, RootCloningToDifferentDocument) {
    const char initial[] = "{ a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6 ] }";

    mmb::Document source(mongo::fromjson(initial));

    // Dirty the 'd' node and parents.
    source.root()["d"]
        .pushBack(source.makeElementInt(mongo::StringData(), 7))
        .transitional_ignore();

    mmb::Document target;

    mmb::Element newElement = target.makeElementWithNewFieldName("X", source.root());
    mongo::Status status = target.root().pushBack(newElement);
    ASSERT_OK(status);
    const char expected[] =
        "{ X : { a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ] } }";

    ASSERT_EQUALS(mongo::fromjson(expected), target);
}

TEST(Document, RootCloningToSameDocument) {
    const char initial[] = "{ a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6 ] }";

    mmb::Document doc(mongo::fromjson(initial));

    // Dirty the 'd' node and parents.
    doc.root()["d"].pushBack(doc.makeElementInt(mongo::StringData(), 7)).transitional_ignore();

    mmb::Element newElement = doc.makeElementWithNewFieldName("X", doc.root());
    mongo::Status status = doc.root().pushBack(newElement);
    ASSERT_OK(status);
    const char expected[] =
        "{ "
        " a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ], "
        "X : { a : 1, b : [ 1, 2, 3 ], c : { 'c' : 'c' }, d : [ 4, 5, 6, 7 ] }"
        "}";

    ASSERT_EQUALS(mongo::fromjson(expected), doc);
}

TEST(Element, PopOpsOnEmpty) {
    mmb::Document doc;
    mmb::Element root = doc.root();
    ASSERT_NOT_OK(root.popFront());
    ASSERT_NOT_OK(root.popBack());
}

TEST(Document, NameOfRootElementIsEmpty) {
    mmb::Document doc;
    // NOTE: You really shouldn't rely on this behavior; this test is mostly for coverage.
    ASSERT_EQUALS(mongo::StringData(), doc.root().getFieldName());
}

TEST(Document, SetValueOnRootFails) {
    mmb::Document doc;
    ASSERT_NOT_OK(doc.root().setValueInt(5));
}

TEST(Document, ValueOfEphemeralObjectElementIsEmpty) {
    mmb::Document doc;
    mmb::Element root = doc.root();
    mmb::Element ephemeralObject = doc.makeElementObject("foo");
    ASSERT_OK(root.pushBack(ephemeralObject));
    ASSERT_FALSE(ephemeralObject.hasValue());
    // NOTE: You really shouldn't rely on this behavior; this test is mostly for coverage.
    ASSERT_BSONELT_EQ(mongo::BSONElement(), ephemeralObject.getValue());
}

TEST(Element, RemovingRemovedElementFails) {
    // Once an Element is removed, you can't remove it again until you re-attach it
    // somewhere. However, its children are still manipulable.
    mmb::Document doc(mongo::fromjson("{ a : { b : 'c' } }"));
    mmb::Element a = doc.root().leftChild();
    ASSERT_TRUE(a.ok());
    ASSERT_OK(a.remove());
    ASSERT_NOT_OK(a.remove());
    mmb::Element b = a.leftChild();
    ASSERT_OK(b.remove());
    ASSERT_NOT_OK(b.remove());
    ASSERT_OK(a.pushBack(b));
    ASSERT_OK(b.remove());
}

namespace {
// Checks that two BSONElements are byte-for-byte identical.
bool identical(const mongo::BSONElement& lhs, const mongo::BSONElement& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    return std::memcmp(lhs.rawdata(), rhs.rawdata(), lhs.size()) == 0;
}
}  // namespace

TEST(TypeSupport, EncodingEquivalenceDouble) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const double value1 = 3.1415926;
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::numberDouble);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendDouble(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::numberDouble);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueDouble());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::numberDouble);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueDouble(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::numberDouble);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceString) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "value1";
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::string);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendString(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::string);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueString());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::string);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueString(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::string);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceObject) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::BSONObj value1 = mongo::fromjson("{ a : 1, b : 2.0, c : 'hello' }");
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::object);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendObject(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::object);
    ASSERT_TRUE(a.hasValue());
    ASSERT_BSONOBJ_EQ(value1, mmb::ConstElement(a).getValueObject());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::object);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueObject(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::object);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceArray) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::BSONObj dummy = (mongo::fromjson("{ x : [ 1, 2.0, 'hello' ] } "));
    const mongo::BSONArray value1(dummy.firstElement().embeddedObject());
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::array);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendArray(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::array);
    ASSERT_TRUE(a.hasValue());
    ASSERT_BSONOBJ_EQ(value1, mmb::ConstElement(a).getValueArray());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::array);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueArray(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::array);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceBinary) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::BinDataType value1 = mongo::newUUID;
    const unsigned char value2[] = {0x00,
                                    0x9D,
                                    0x15,
                                    0xA3,
                                    0x3B,
                                    0xCC,
                                    0x46,
                                    0x60,
                                    0x90,
                                    0x45,
                                    0xEF,
                                    0x54,
                                    0x77,
                                    0x8A,
                                    0x87,
                                    0x0C};
    builder.appendBinData(name, sizeof(value2), value1, &value2[0]);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::binData);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendBinary(name, sizeof(value2), value1, &value2[0]));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::binData);
    ASSERT_TRUE(a.hasValue());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::binData);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueBinary(sizeof(value2), value1, &value2[0]).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::binData);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceUndefined) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    builder.appendUndefined(name);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::undefined);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendUndefined(name));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::undefined);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(mmb::ConstElement(a).isValueUndefined());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::undefined);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueUndefined().transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::undefined);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceOID) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::OID value1 = mongo::OID::gen();
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::oid);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendOID(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::oid);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueOID());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::oid);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueOID(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::oid);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceBoolean) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const bool value1 = true;
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::boolean);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendBool(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::boolean);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueBool());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::boolean);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueBool(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::boolean);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceDate) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::Date_t value1 = mongo::Date_t::now();
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::date);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendDate(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::date);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueDate());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::date);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueDate(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::date);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceNull) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    builder.appendNull(name);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::null);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::null);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(mmb::ConstElement(a).isValueNull());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::null);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendUndefined(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueNull().transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::null);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceRegex) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "some_regex_data";
    const std::string value2 = "flags";
    builder.appendRegex(name, value1, value2);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::regEx);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendRegex(name, value1, value2));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::regEx);
    ASSERT_TRUE(a.hasValue());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::regEx);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueRegex(value1, value2).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::regEx);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceDBRef) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "some_ns";
    const mongo::OID value2 = mongo::OID::gen();
    builder.appendDBRef(name, value1, value2);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::dbRef);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendDBRef(name, value1, value2));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::dbRef);
    ASSERT_TRUE(a.hasValue());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::dbRef);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueDBRef(value1, value2).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::dbRef);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceCode) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "{ print 4; }";
    builder.appendCode(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::code);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendCode(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::code);
    ASSERT_TRUE(a.hasValue());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::code);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueCode(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::code);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceSymbol) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "#symbol";
    builder.appendSymbol(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::symbol);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendSymbol(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::symbol);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueSymbol());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::symbol);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueSymbol(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::symbol);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceCodeWithScope) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const std::string value1 = "print x;";
    const mongo::BSONObj value2 = mongo::fromjson("{ x : 4 }");
    builder.appendCodeWScope(name, value1, value2);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::codeWScope);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendCodeWithScope(name, value1, value2));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::codeWScope);
    ASSERT_TRUE(a.hasValue());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::codeWScope);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueCodeWithScope(value1, value2).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::codeWScope);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceInt) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const int value1 = true;
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::numberInt);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendInt(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::numberInt);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueInt());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::numberInt);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueInt(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::numberInt);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceTimestamp) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::Timestamp value1 = mongo::Timestamp(mongo::Date_t::now());
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == BSONType::timestamp);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendTimestamp(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), BSONType::timestamp);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(value1 == mmb::ConstElement(a).getValueTimestamp());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), BSONType::timestamp);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueTimestamp(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), BSONType::timestamp);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceLong) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const long long value1 = 420000000000000LL;
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::numberLong);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendLong(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::numberLong);
    ASSERT_TRUE(a.hasValue());
    ASSERT_EQUALS(value1, mmb::ConstElement(a).getValueLong());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::numberLong);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueLong(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::numberLong);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceDecimal) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    const mongo::Decimal128 value1 = mongo::Decimal128(2);
    builder.append(name, value1);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::numberDecimal);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendDecimal(name, value1));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::numberDecimal);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(value1.isEqual(mmb::ConstElement(a).getValueDecimal()));

    // Construct via call passong BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::numberDecimal);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueDecimal(value1).transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::numberDecimal);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceMinKey) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    builder.appendMinKey(name);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::minKey);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendMinKey(name));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::minKey);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(mmb::ConstElement(a).isValueMinKey());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::minKey);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueMinKey().transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::minKey);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}

TEST(TypeSupport, EncodingEquivalenceMaxKey) {
    mongo::BSONObjBuilder builder;
    const char name[] = "thing";
    builder.appendMaxKey(name);
    mongo::BSONObj source = builder.done();
    const mongo::BSONElement thing = source.firstElement();
    ASSERT_TRUE(thing.type() == mongo::BSONType::maxKey);

    mmb::Document doc;

    // Construct via direct call to append/make
    ASSERT_OK(doc.root().appendMaxKey(name));
    mmb::Element a = doc.root().rightChild();
    ASSERT_TRUE(a.ok());
    ASSERT_EQUALS(a.getType(), mongo::BSONType::maxKey);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(mmb::ConstElement(a).isValueMaxKey());

    // Construct via call passing BSON element
    ASSERT_OK(doc.root().appendElement(thing));
    mmb::Element b = doc.root().rightChild();
    ASSERT_TRUE(b.ok());
    ASSERT_EQUALS(b.getType(), mongo::BSONType::maxKey);
    ASSERT_TRUE(b.hasValue());

    // Construct via setValue call.
    ASSERT_OK(doc.root().appendNull(name));
    mmb::Element c = doc.root().rightChild();
    ASSERT_TRUE(c.ok());
    c.setValueMaxKey().transitional_ignore();
    ASSERT_EQUALS(c.getType(), mongo::BSONType::maxKey);
    ASSERT_TRUE(c.hasValue());

    // Ensure identity:
    ASSERT_TRUE(identical(thing, mmb::ConstElement(a).getValue()));
    ASSERT_TRUE(identical(a.getValue(), b.getValue()));
    ASSERT_TRUE(identical(b.getValue(), c.getValue()));
}


TEST(InputValidation, IllegalFieldName) {
    // Test that using field names with null characters throw an assertion.
    mmb::Document doc;
    std::string embeddedNullStr("st\0r", 4);

    ASSERT_THROWS_CODE(doc.makeElementObject(embeddedNullStr), DBException, 9527900);
    ASSERT_THROWS_CODE(doc.makeElementArray(embeddedNullStr), DBException, 9527900);
    ASSERT_THROWS_CODE(doc.makeElementDouble(embeddedNullStr, 1.0), DBException, 9527900);
    ASSERT_THROWS_CODE(doc.makeElementBool(embeddedNullStr, true), DBException, 9527900);
    ASSERT_THROWS_CODE(
        doc.makeElementDecimal(embeddedNullStr, Decimal128(3)), DBException, 9527900);

    mmb::Element subObj = doc.makeElementObject("subObj");
    ASSERT_OK(doc.root().pushBack(subObj));

    ASSERT_THROWS_CODE(
        subObj.getDocument().makeElementObject(embeddedNullStr), DBException, 9527900);
    ASSERT_THROWS_CODE(subObj.rename(embeddedNullStr), DBException, 9527900);
}

TEST(Document, ManipulateComplexObjInLeafHeap) {
    // Test that an object with complex substructure that lives in the leaf builder can be
    // manipulated in the same way as an object with complex substructure that lives
    // freely.
    mmb::Document doc;
    static const char inJson[] = "{ a: 1, b: 2, d : ['w', 'x', 'y', 'z'] }";
    mmb::Element embedded = doc.makeElementObject("embedded", mongo::fromjson(inJson));
    ASSERT_OK(doc.root().pushBack(embedded));
    mmb::Element free = doc.makeElementObject("free");
    ASSERT_OK(doc.root().pushBack(free));

    mmb::Element e_a = embedded.leftChild();
    ASSERT_TRUE(e_a.ok());
    ASSERT_EQUALS("a", e_a.getFieldName());
    mmb::Element e_b = e_a.rightSibling();
    ASSERT_TRUE(e_b.ok());
    ASSERT_EQUALS("b", e_b.getFieldName());

    mmb::Element new_c = doc.makeElementDouble("c", 2.0);
    ASSERT_TRUE(new_c.ok());
    ASSERT_OK(e_b.addSiblingRight(new_c));

    mmb::Element e_d = new_c.rightSibling();
    ASSERT_TRUE(e_d.ok());
    ASSERT_EQUALS("d", e_d.getFieldName());

    mmb::Element e_d_0 = e_d.leftChild();
    ASSERT_TRUE(e_d_0.ok());

    mmb::Element e_d_1 = e_d_0.rightSibling();
    ASSERT_TRUE(e_d_1.ok());

    mmb::Element e_d_2 = e_d_1.rightSibling();
    ASSERT_TRUE(e_d_2.ok());

    ASSERT_OK(e_d_1.remove());

    static const char outJson[] =
        "{ embedded: { a: 1, b: 2, c: 2.0, d : ['w', 'y', 'z'] }, free: {} }";
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), doc.getObject());
}

TEST(DocumentInPlace, EphemeralDocumentsDoNotUseInPlaceMode) {
    mmb::Document doc;
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsHonored1) {
    mongo::BSONObj obj;
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    doc.disableInPlaceUpdates();
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsHonored2) {
    mongo::BSONObj obj;
    mmb::Document doc(obj, mmb::Document::kInPlaceDisabled);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    doc.disableInPlaceUpdates();
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeWorksWithNoMutations) {
    mongo::BSONObj obj;
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    const char* source = nullptr;
    DamageVector damages;
    ASSERT_TRUE(damages.empty());
    doc.getInPlaceUpdates(&damages, &source);
    ASSERT_TRUE(damages.empty());
    ASSERT_NOT_EQUALS(static_cast<const char*>(nullptr), source);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByAddSiblingLeft) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    mmb::Element newElt = doc.makeElementInt("bar", 42);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().leftChild().addSiblingLeft(newElt));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByAddSiblingRight) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    mmb::Element newElt = doc.makeElementInt("bar", 42);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().leftChild().addSiblingRight(newElt));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByRemove) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().leftChild().remove());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

// NOTE: Someday, we may do in-place renames, but renaming 'foo' to 'foobar' will never
// work because the sizes don't match. Validate that this disables in-place updates.
TEST(DocumentInPlace, InPlaceModeIsDisabledByRename) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().leftChild().rename("foobar"));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByPushFront) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    mmb::Element newElt = doc.makeElementInt("bar", 42);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().pushFront(newElt));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByPushBack) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    mmb::Element newElt = doc.makeElementInt("bar", 42);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().pushBack(newElt));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByPopFront) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().popFront());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, InPlaceModeIsDisabledByPopBack) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_OK(doc.root().popBack());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, ReserveDamageEventsIsAlwaysSafeToCall) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    doc.reserveDamageEvents(10);
    doc.disableInPlaceUpdates();
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    doc.reserveDamageEvents(10);
}

TEST(DocumentInPlace, GettingInPlaceUpdatesWhenDisabledClearsArguments) {
    mongo::BSONObj obj = mongo::fromjson("{ foo : 'foo' }");
    mmb::Document doc(obj, mmb::Document::kInPlaceDisabled);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    DamageVector damages;
    const DamageEvent event{};
    damages.push_back(event);
    const char* source = "foo";
    ASSERT_FALSE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_TRUE(damages.empty());
    ASSERT_EQUALS(static_cast<const char*>(nullptr), source);

    damages.push_back(event);
    source = "bar";
    size_t size = 1;
    ASSERT_FALSE(doc.getInPlaceUpdates(&damages, &source, &size));
    ASSERT_TRUE(damages.empty());
    ASSERT_EQUALS(static_cast<const char*>(nullptr), source);
    ASSERT_EQUALS(0U, size);
}

// This isn't a great test since we aren't testing all possible combinations of compatible
// and incompatible sets, but since all setValueX calls decay to the internal setValue, we
// can be pretty sure that this will at least check the logic somewhat.
TEST(DocumentInPlace, InPlaceModeIsDisabledByIncompatibleSetValue) {
    mongo::BSONObj obj(mongo::fromjson("{ foo : false }"));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    mmb::Element foo = doc.root().leftChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(foo.setValueString("foo"));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(DocumentInPlace, DisablingInPlaceDoesNotDiscardUpdates) {
    mongo::BSONObj obj(mongo::fromjson("{ foo : false, bar : true }"));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    mmb::Element foo = doc.root().leftChild();
    ASSERT_TRUE(foo.ok());
    ASSERT_OK(foo.setValueBool(true));
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    mmb::Element bar = doc.root().rightChild();
    ASSERT_TRUE(bar.ok());
    ASSERT_OK(bar.setValueBool(false));
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    ASSERT_OK(doc.root().appendString("baz", "baz"));
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    static const char outJson[] = "{ foo : true, bar : false, baz : 'baz' }";
    ASSERT_BSONOBJ_EQ(mongo::fromjson(outJson), doc.getObject());
}

TEST(DocumentInPlace, StringLifecycle) {
    mongo::BSONObj obj(mongo::fromjson("{ x : 'foo' }"));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueString("bar").transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::string));
    ASSERT_EQUALS("bar", x.getValueString());

    // TODO: When in-place updates for leaf elements is implemented, add tests here.
}

TEST(DocumentInPlace, BinDataLifecycle) {
    const char kData1[] = "\x01\x02\x03\x04\x05\x06";
    const char kData2[] = "\x10\x20\x30\x40\x50\x60";

    const mongo::BSONBinData binData1(kData1, sizeof(kData1) - 1, mongo::BinDataGeneral);
    const mongo::BSONBinData binData2(kData2, sizeof(kData2) - 1, mongo::bdtCustom);

    mongo::BSONObj obj(BSON("x" << binData1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueBinary(binData2.length, binData2.type, binData2.data).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(BSONType::binData));

    mongo::BSONElement value = x.getValue();
    ASSERT_EQUALS(binData2.type, value.binDataType());
    int len = 0;
    const char* const data = value.binDataClean(len);
    ASSERT_EQUALS(binData2.length, len);
    ASSERT_EQUALS(0, std::memcmp(data, kData2, len));

    // TODO: When in-place updates for leaf elements is implemented, add tests here.
}

TEST(DocumentInPlace, OIDLifecycle) {
    const mongo::OID oid1 = mongo::OID::gen();
    const mongo::OID oid2 = mongo::OID::gen();
    ASSERT_NOT_EQUALS(oid1, oid2);

    mongo::BSONObj obj(BSON("x" << oid1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueOID(oid2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(BSONType::oid));
    ASSERT_EQUALS(oid2, x.getValueOID());

    // TODO: When in-place updates for leaf elements is implemented, add tests here.
}

TEST(DocumentInPlace, BooleanLifecycle) {
    mongo::BSONObj obj(mongo::fromjson("{ x : false }"));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueBool(false).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::boolean));
    ASSERT_EQUALS(false, x.getValueBool());

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueBool(true);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::boolean));
    // ASSERT_EQUALS(true, x.getValueBool());
}

TEST(DocumentInPlace, DateLifecycle) {
    mongo::BSONObj obj(BSON("x" << mongo::Date_t::fromMillisSinceEpoch(1000)));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueDate(mongo::Date_t::fromMillisSinceEpoch(20000)).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::date));
    ASSERT_EQUALS(mongo::Date_t::fromMillisSinceEpoch(20000), x.getValueDate());

    // TODO: When in-place updates for leaf elements is implemented, add tests here.
}

TEST(DocumentInPlace, NumberIntLifecycle) {
    const int value1 = 42;
    const int value2 = 3;
    mongo::BSONObj obj(BSON("x" << value1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueInt(value2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::numberInt));
    ASSERT_EQUALS(value2, x.getValueInt());

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueInt(value1);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::numberInt));
    // ASSERT_EQUALS(value1, x.getValueInt());
}

TEST(DocumentInPlace, TimestampLifecycle) {
    mongo::BSONObj obj(BSON("x" << mongo::Timestamp(mongo::Date_t::fromMillisSinceEpoch(1000))));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueTimestamp(mongo::Timestamp(mongo::Date_t::fromMillisSinceEpoch(20000)))
        .transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::timestamp));
    ASSERT_TRUE(mongo::Timestamp(20000U) == x.getValueTimestamp());

    // TODO: When in-place updates for leaf elements is implemented, add tests here.
}

TEST(DocumentInPlace, NumberLongLifecycle) {
    const long long value1 = 42;
    const long long value2 = 3;

    mongo::BSONObj obj(BSON("x" << value1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueLong(value2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::numberLong));
    ASSERT_EQUALS(value2, x.getValueLong());

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueLong(value1);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::numberLong));
    // ASSERT_EQUALS(value1, x.getValueLong());
}

TEST(DocumentInPlace, NumberDoubleLifecycle) {
    const double value1 = 32.0;
    const double value2 = 2.0;

    mongo::BSONObj obj(BSON("x" << value1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueDouble(value2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::numberDouble));
    ASSERT_EQUALS(value2, x.getValueDouble());

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueDouble(value1);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::numberDouble));
    // ASSERT_EQUALS(value1, x.getValueDouble());
}

TEST(DocumentInPlace, NumberDecimalLifecycle) {
    const mongo::Decimal128 value1 = mongo::Decimal128(32);
    const mongo::Decimal128 value2 = mongo::Decimal128(2);

    mongo::BSONObj obj(BSON("x" << value1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueDecimal(value2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    ASSERT_EQUALS(1U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::numberDecimal));
    ASSERT_TRUE(value2.isEqual(x.getValueDecimal()));

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueDecimal(value1);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::numberDecimal));
    // ASSERT_TRUE(value1.isEqual(x.getValueDecimal()));
}

// Doubles and longs are the same size, 8 bytes, so we should be able to do in-place
// updates between them.
TEST(DocumentInPlace, DoubleToLongAndBack) {
    const double value1 = 32.0;
    const long long value2 = 42;

    mongo::BSONObj obj(BSON("x" << value1));
    mmb::Document doc(obj, mmb::Document::kInPlaceEnabled);

    mmb::Element x = doc.root().leftChild();

    DamageVector damages;
    const char* source = nullptr;

    x.setValueLong(value2).transitional_ignore();
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // We changed the type, so we get an extra damage event.
    ASSERT_EQUALS(2U, damages.size());
    apply(&obj, damages, source);
    ASSERT_TRUE(x.hasValue());
    ASSERT_TRUE(x.isType(mongo::BSONType::numberLong));
    ASSERT_EQUALS(value2, x.getValueLong());

    // TODO: Re-enable when in-place updates to leaf elements is supported
    // x.setValueDouble(value1);
    // ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source));
    // apply(&obj, damages, source);
    // ASSERT_TRUE(x.hasValue());
    // ASSERT_TRUE(x.isType(mongo::BSONType::numberDouble));
    // ASSERT_EQUALS(value1, x.getValueDouble());
}

TEST(DocumentComparison, SimpleComparison) {
    const mongo::BSONObj obj =
        mongo::fromjson("{ a : 'a', b : ['b', 'b', 'b'], c : { one : 1.0 } }");

    const mmb::Document doc1(obj.getOwned());
    ASSERT_EQUALS(0, doc1.compareWithBSONObj(obj, nullptr));
    const mmb::Document doc2(obj.getOwned());
    ASSERT_EQUALS(0, doc1.compareWith(doc2, nullptr));
    ASSERT_EQUALS(0, doc2.compareWith(doc1, nullptr));
}

TEST(DocumentComparison, SimpleComparisonWithDeserializedElements) {
    const mongo::BSONObj obj =
        mongo::fromjson("{ a : 'a', b : ['b', 'b', 'b'], c : { one : 1.0 } }");

    // Perform an operation on 'b' that doesn't change the serialized value, but
    // deserializes the node.
    mmb::Document doc1(obj.getOwned());
    const mmb::Document doc1Copy(obj.getOwned());
    mmb::Element b = doc1.root()["b"];
    ASSERT_TRUE(b.ok());
    mmb::Element b0 = b[0];
    ASSERT_TRUE(b0.ok());
    ASSERT_OK(b0.remove());
    ASSERT_OK(b.pushBack(b0));
    // Ensure that it compares correctly against the source object.
    ASSERT_EQUALS(0, doc1.compareWithBSONObj(obj, nullptr));
    // Ensure that it compares correctly against a pristine document.
    ASSERT_EQUALS(0, doc1.compareWith(doc1Copy, nullptr));
    ASSERT_EQUALS(0, doc1Copy.compareWith(doc1, nullptr));

    // Perform an operation on 'c' that doesn't change the serialized value, but
    // deserializes the node.
    mmb::Document doc2(obj.getOwned());
    const mmb::Document doc2Copy(obj.getOwned());
    mmb::Element c = doc2.root()["c"];
    ASSERT_TRUE(c.ok());
    mmb::Element c1 = c.leftChild();
    ASSERT_TRUE(c1.ok());
    ASSERT_OK(c1.remove());
    ASSERT_OK(c.pushBack(c1));
    // Ensure that it compares correctly against the source object
    ASSERT_EQUALS(0, doc2.compareWithBSONObj(obj, nullptr));
    // Ensure that it compares correctly against a pristine document.
    ASSERT_EQUALS(0, doc2.compareWith(doc2Copy, nullptr));
    ASSERT_EQUALS(0, doc2Copy.compareWith(doc2, nullptr));

    // Ensure that the two deserialized documents compare with each other correctly.
    ASSERT_EQUALS(0, doc1.compareWith(doc2, nullptr));
    ASSERT_EQUALS(0, doc2.compareWith(doc1, nullptr));
}

TEST(DocumentComparison, DocumentCompareWithRespectsCollation) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{a: 'foo'}"));
    const mmb::Document doc2(mongo::fromjson("{a: 'bar'}"));
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, doc1.compareWith(doc2, nullptr, true));
    ASSERT_EQ(0, doc1.compareWith(doc2, &collator, true));
}

TEST(DocumentComparison, DocumentCompareWithBSONObjRespectsCollation) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{a: 'foo'}"));
    const mongo::BSONObj doc2 = mongo::fromjson("{a: 'bar'}");
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, doc1.compareWithBSONObj(doc2, nullptr, true));
    ASSERT_EQ(0, doc1.compareWithBSONObj(doc2, &collator, true));
}

TEST(DocumentComparison, ElementCompareWithElementRespectsCollator) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{a: 'foo'}"));
    const mmb::Document doc2(mongo::fromjson("{a: 'bar'}"));
    const mmb::ConstElement element1 = doc1.root().leftChild();
    const mmb::ConstElement element2 = doc2.root().leftChild();
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, element1.compareWithElement(element2, nullptr, true));
    ASSERT_EQ(0, element1.compareWithElement(element2, &collator, true));
}

TEST(DocumentComparison, ElementCompareWithBSONElementRespectsCollator) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{a: 'foo'}"));
    const mongo::BSONObj doc2 = mongo::fromjson("{a: 'bar'}");
    const mmb::ConstElement element1 = doc1.root().leftChild();
    const mongo::BSONElement element2 = doc2["a"];
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, element1.compareWithBSONElement(element2, nullptr, true));
    ASSERT_EQ(0, element1.compareWithBSONElement(element2, &collator, true));
}

TEST(DocumentComparison, ElementCompareWithBSONObjRespectsCollator) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{b: {c: 'foo'}}"));
    const mongo::BSONObj doc2 = mongo::fromjson("{c: 'bar'}");
    const mmb::ConstElement element1 = doc1.root().leftChild();
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, element1.compareWithBSONObj(doc2, nullptr, true));
    ASSERT_EQ(0, element1.compareWithBSONObj(doc2, &collator, true));
}

TEST(DocumentComparison, DocumentCompareWithRespectsCollationRecursively) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    const mmb::Document doc1(mongo::fromjson("{a: [{b: 'foo'}, {b: 'bar'}]}"));
    const mmb::Document doc2(mongo::fromjson("{a: [{b: 'notFoo'}, {b: 'notBar'}]}"));
    // Pass true to indicate that we should compare field names. The two documents should be unequal
    // without the collator, but equal when using the "always equal" collator.
    ASSERT_NE(0, doc1.compareWith(doc2, nullptr, true));
    ASSERT_EQ(0, doc1.compareWith(doc2, &collator, true));
}

TEST(DocumentComparison, DocumentCompareWithRespectsCollationWithDeserializedElement) {
    mongo::CollatorInterfaceMock collator(mongo::CollatorInterfaceMock::MockType::kAlwaysEqual);
    mmb::Document doc1(mongo::fromjson("{a: ['foo', 'foo']}"));
    mmb::Document doc2(mongo::fromjson("{a: ['bar', 'bar']}"));

    // With the always equal collator, the documents should start out comparing equal.
    ASSERT_EQ(0, doc1.compareWith(doc2, &collator, true));
    ASSERT_EQ(0, doc2.compareWith(doc1, &collator, true));

    // They should still be equal after causing deserialization of one of the leaf elements of
    // 'doc1'.
    {
        mmb::Element elementA = doc1.root()["a"];
        ASSERT_TRUE(elementA.ok());
        mmb::Element elementA0 = elementA[0];
        ASSERT_TRUE(elementA0.ok());
        ASSERT_OK(elementA0.remove());
        ASSERT_OK(elementA.pushBack(elementA0));
        ASSERT_EQ(0, doc1.compareWith(doc2, &collator, true));
        ASSERT_EQ(0, doc2.compareWith(doc1, &collator, true));
    }

    // And they should remain equal after doing the same to 'doc2'.
    {
        mmb::Element elementA = doc2.root()["a"];
        ASSERT_TRUE(elementA.ok());
        mmb::Element elementA0 = elementA[0];
        ASSERT_TRUE(elementA0.ok());
        ASSERT_OK(elementA0.remove());
        ASSERT_OK(elementA.pushBack(elementA0));
        ASSERT_EQ(0, doc1.compareWith(doc2, &collator, true));
        ASSERT_EQ(0, doc2.compareWith(doc1, &collator, true));
    }
}

TEST(UnorderedEqualityChecker, Identical) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");

    const mongo::BSONObj b2 = b1.getOwned();

    ASSERT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, DifferentValuesAreNotEqual) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 4 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, DifferentTypesAreNotEqual) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 = mongo::fromjson(
        "{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : '2', z : 3 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, DifferentFieldNamesAreNotEqual) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, Y : 2, z : 3 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, MissingFieldsInObjectAreNotEqual) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, z : 3 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, ObjectOrderingIsNotConsidered) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 = mongo::fromjson(
        "{ b : { y : 2, z : 3 , x : 1  }, a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ] }");

    ASSERT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, ArrayOrderingIsConsidered) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");

    const mongo::BSONObj b2 =
        mongo::fromjson("{ a : [ 1, { 'a' : 'b', 'x' : 'y' }, 2 ], b : { x : 1, y : 2, z : 3 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(UnorderedEqualityChecker, MissingItemsInArrayAreNotEqual) {
    const mongo::BSONObj b1 =
        mongo::fromjson("{ a : [ 1, 2, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, y : 2, z : 3 } }");
    const mongo::BSONObj b2 =
        mongo::fromjson("{ a : [ 1, { 'a' : 'b', 'x' : 'y' } ], b : { x : 1, z : 3 } }");

    ASSERT_NOT_EQUALS(mmb::unordered(b1), mmb::unordered(b2));
}

TEST(SERVER_36024, RegressionCheck) {
    mongo::BSONObj obj;
    mmb::Document doc(obj, mmb::Document::kInPlaceDisabled);

    mmb::Element root = doc.root();
    auto array = doc.makeElementArray("foo");
    ASSERT_OK(root.pushBack(array));

    mmb::Element field = root.findFirstChildNamed("foo");
    ASSERT_OK(field.setValueInt(1));
}

}  // namespace
