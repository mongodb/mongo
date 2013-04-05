/* Copyright 2012 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/document.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/timer.h"

#include "mongo/client/dbclientinterface.h"

namespace {

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

        mmb::Element e8 = doc.makeElementInt("", 100);
        ASSERT_EQUALS(100, e8.getValueInt());
        ASSERT_EQUALS(e1[0].setValueElement(&e8), mongo::Status::OK());
        ASSERT_EQUALS(100, e8.getValueInt());
        ASSERT_EQUALS(100, e1[0].getValueInt());
        ASSERT_EQUALS(100, e1.leftChild().getValueInt());
        ASSERT_EQUALS(size_t(1), mmb::countChildren(e1));
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());

        ASSERT_EQUALS(size_t(0), mmb::countChildren(e1));
        ASSERT_FALSE(e1[0].ok());
        ASSERT_FALSE(e1[1].ok());
    }

    TEST(Element, setters) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementNull("t0");

        t0.setValueBool(true);
        ASSERT_EQUALS(mongo::Bool, t0.getType());

        t0.setValueInt(12345);
        ASSERT_EQUALS(mongo::NumberInt, t0.getType());

        t0.setValueLong(12345LL);
        ASSERT_EQUALS(mongo::NumberLong, t0.getType());

        t0.setValueTimestamp(mongo::OpTime());
        ASSERT_EQUALS(mongo::Timestamp, t0.getType());

        t0.setValueDate(12345LL);
        ASSERT_EQUALS(mongo::Date, t0.getType());

        t0.setValueDouble(123.45);
        ASSERT_EQUALS(mongo::NumberDouble, t0.getType());

        t0.setValueOID(mongo::OID("47cc67093475061e3d95369d"));
        ASSERT_EQUALS(mongo::jstOID, t0.getType());

        t0.setValueRegex("[a-zA-Z]?", "");
        ASSERT_EQUALS(mongo::RegEx, t0.getType());

        t0.setValueString("foo bar baz");
        ASSERT_EQUALS(mongo::String, t0.getType());
    }

    TEST(TimestampType, createElement) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementTimestamp("t0", mongo::OpTime());
        ASSERT(mongo::OpTime() == t0.getValueTimestamp());

        mmb::Element t1 = doc.makeElementTimestamp("t1", mongo::OpTime(123, 456));
        ASSERT(mongo::OpTime(123, 456) == t1.getValueTimestamp());
    }

    TEST(TimestampType, setElement) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementTimestamp("t0", mongo::OpTime());
        t0.setValueTimestamp(mongo::OpTime(123, 456));
        ASSERT(mongo::OpTime(123, 456) == t0.getValueTimestamp());

        // Try setting to other types and back to OpTime
        t0.setValueLong(1234567890);
        ASSERT_EQUALS(1234567890LL, t0.getValueLong());
        t0.setValueTimestamp(mongo::OpTime(789, 321));
        ASSERT(mongo::OpTime(789, 321) == t0.getValueTimestamp());

        t0.setValueString("foo bar baz");
        ASSERT_EQUALS("foo bar baz", t0.getValueString());
        t0.setValueTimestamp(mongo::OpTime(9876, 5432));
        ASSERT(mongo::OpTime(9876, 5432) == t0.getValueTimestamp());
    }

    TEST(TimestampType, appendElement) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementObject("e0");
        t0.appendTimestamp("a timestamp field", mongo::OpTime(1352151971, 471));

        mmb::Element it =
            mmb::findFirstChildNamed(t0, "a timestamp field");
        ASSERT_TRUE(it.ok());
        ASSERT(mongo::OpTime(1352151971, 471) == it.getValueTimestamp());
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
        ASSERT_EQUALS(num, 1234567890);

        t0.setValueLong(1234567890LL);
        ASSERT_EQUALS(1234567890LL, t0.getValueLong());
        num = t0.getValueSafeNum();
        ASSERT_EQUALS(num, 1234567890LL);

        t0.setValueDouble(123.456789);
        ASSERT_EQUALS(123.456789, t0.getValueDouble());
        num = t0.getValueSafeNum();
        ASSERT_EQUALS(num, 123.456789);
    }

    TEST(SafeNumType, setSafeNum) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementSafeNum("t0", mongo::SafeNum(123456));
        t0.setValueSafeNum(mongo::SafeNum(654321));
        ASSERT_EQUALS(mongo::SafeNum(654321), t0.getValueSafeNum());

        // Try setting to other types and back to SafeNum
        t0.setValueLong(1234567890);
        ASSERT_EQUALS(1234567890LL, t0.getValueLong());
        t0.setValueSafeNum(mongo::SafeNum(1234567890));
        ASSERT_EQUALS(mongo::SafeNum(1234567890), t0.getValueSafeNum());

        t0.setValueString("foo bar baz");

        mongo::StringData left = "foo bar baz";
        mongo::StringData right = t0.getValueString();
        ASSERT_EQUALS(left, right);

        ASSERT_EQUALS(mongo::StringData("foo bar baz"), t0.getValueString());
        t0.setValueSafeNum(mongo::SafeNum(12345));
        ASSERT_EQUALS(mongo::SafeNum(12345), t0.getValueSafeNum());
    }

    TEST(SafeNumType, appendElement) {
        mmb::Document doc;

        mmb::Element t0 = doc.makeElementObject("e0");
        t0.appendSafeNum("a timestamp field", mongo::SafeNum(1352151971LL));

        mmb::Element it = findFirstChildNamed(t0, "a timestamp field");
        ASSERT_TRUE(it.ok());
        ASSERT_EQUALS(mongo::SafeNum(1352151971LL), it.getValueSafeNum());
    }

    TEST(OIDType, getOidValue) {
        mmb::Document doc;
        mmb::Element t0 = doc.makeElementObject("e0");
        const mongo::OID generated = mongo::OID::gen();
        t0.appendOID("myOid", generated);
        mmb::Element it = findFirstChildNamed(t0, "myOid");
        const mongo::OID recovered = mongo::OID(it.getValueOID());
        ASSERT_EQUALS(generated, recovered);
    }

    TEST(OIDType, nullOID) {
        mmb::Document doc;
        mmb::Element t0 = doc.makeElementObject("e0");
        const mongo::OID withNull("50a9c82263e413ad0028faad");
        t0.appendOID("myOid", withNull);
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

    TEST(Serialization, RoundTrip) {
        mongo::BSONObj obj = mongo::fromjson(jsonSample);
        mmb::Document doc(obj.copy());
        mongo::BSONObj built = doc.getObject();
        ASSERT_EQUALS(obj, built);
    }

    TEST(Documentation, Example1) {

        // Create a new document
        mmb::Document doc;
        ASSERT_EQUALS(mongo::fromjson("{}"),
                      doc.getObject());

        // Get the root of the document.
        mmb::Element root = doc.root();

        // Create a new mongo::NumberInt typed Element to represent life, the universe, and
        // everything, then push that Element into the root object, making it a child of root.
        mmb::Element e0 = doc.makeElementInt("ltuae", 42);
        ASSERT_OK(root.pushBack(e0));
        ASSERT_EQUALS(mongo::fromjson("{ ltuae : 42 }"),
                      doc.getObject());

        // Create a new empty mongo::Object-typed Element named 'magic', and push it back as a
        // child of the root, making it a sibling of e0.
        mmb::Element e1 = doc.makeElementObject("magic");
        ASSERT_OK(root.pushBack(e1));
        ASSERT_EQUALS(mongo::fromjson("{ ltuae : 42, magic : {} }"),
                      doc.getObject());

        // Create a new mongo::NumberDouble typed Element to represent Pi, and insert it as child
        // of the new object we just created.
        mmb::Element e3 = doc.makeElementDouble("pi", 3.14);
        ASSERT_OK(e1.pushBack(e3));
        ASSERT_EQUALS(mongo::fromjson("{ ltuae : 42, magic : { pi : 3.14 } }"),
                      doc.getObject());

        // Create a new mongo::NumberDouble to represent Plancks constant in electrovolt
        // micrometers, and add it as a child of the 'magic' object.
        mmb::Element e4 = doc.makeElementDouble("hbar", 1.239);
        ASSERT_OK(e1.pushBack(e4));
        ASSERT_EQUALS(mongo::fromjson("{ ltuae : 42, magic : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

        // Rename the parent element of 'hbar' to be 'constants'.
        ASSERT_OK(e4.parent().rename("constants"));
        ASSERT_EQUALS(mongo::fromjson("{ ltuae : 42, constants : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

        // Rename 'ltuae' to 'answer' by accessing it as the root objects left child.
        ASSERT_OK(doc.root().leftChild().rename("answer"));
        ASSERT_EQUALS(mongo::fromjson("{ answer : 42, constants : { pi : 3.14, hbar : 1.239 } }"),
                      doc.getObject());

        // Sort the constants by name.
        mmb::sortChildren(doc.root().rightChild(), mmb::FieldNameLessThan());
        ASSERT_EQUALS(mongo::fromjson("{ answer : 42, constants : { hbar : 1.239, pi : 3.14 } }"),
                      doc.getObject());
    }

    TEST(Documentation, Example2) {

        static const char inJson[] =
            "{"
            "  'whale': { 'alive': true, 'dv': -9.8, 'height': 50, attrs : [ 'big' ] },"
            "  'petunias': { 'alive': true, 'dv': -9.8, 'height': 50 } "
            "}";
        mongo::BSONObj obj = mongo::fromjson(inJson);

        // Create a new document representing bacBSONObj with the above contents.
        mmb::Document doc(obj);

        // The whale hits the planet and dies.
        mmb::Element whale = mmb::findFirstChildNamed(doc.root(), "whale");
        ASSERT_TRUE(whale.ok());
        // Find the 'dv' field in the whale.
        mmb::Element whale_deltav = mmb::findFirstChildNamed(whale, "dv");
        ASSERT_TRUE(whale_deltav.ok());
        // Set the dv field to zero.
        whale_deltav.setValueDouble(0.0);
        // Find the 'height' field in the whale.
        mmb::Element whale_height = mmb::findFirstChildNamed(whale, "height");
        ASSERT_TRUE(whale_height.ok());
        // Set the height field to zero.
        whale_deltav.setValueDouble(0);
        // Find the 'alive' field, and set it to false.
        mmb::Element whale_alive = mmb::findFirstChildNamed(whale, "alive");
        ASSERT_TRUE(whale_alive.ok());
        whale_alive.setValueBool(false);

        // The petunias survive, update its fields much like we did above.
        mmb::Element petunias = mmb::findFirstChildNamed(doc.root(), "petunias");
        ASSERT_TRUE(petunias.ok());
        mmb::Element petunias_deltav = mmb::findFirstChildNamed(petunias, "dv");
        ASSERT_TRUE(petunias_deltav.ok());
        petunias_deltav.setValueDouble(0.0);
        mmb::Element petunias_height = mmb::findFirstChildNamed(petunias, "height");
        ASSERT_TRUE(petunias_height.ok());
        petunias_deltav.setValueDouble(0);

        // Replace the whale by its wreckage, saving only its attributes:
        // Construct a new mongo::Object element for the ex-whale.
        mmb::Element ex_whale = doc.makeElementObject("ex-whale");
        ASSERT_OK(doc.root().pushBack(ex_whale));
        // Find the attributes of the old 'whale' element.
        mmb::Element whale_attrs = mmb::findFirstChildNamed(whale, "attrs");
        // Remove the attributes from the whale (they remain valid, but detached).
        whale_attrs.remove();
        // Insert the attributes into the ex-whale.
        ex_whale.pushBack(whale_attrs);
        // Remove the whale object.
        whale.remove();

        static const char outJson[] =
            "{"
            "    'petunias': { 'alive': true, 'dv': 0.0, 'height': 50 },"
            "    'ex-whale': { 'attrs': [ 'big' ] } })"
            "}";

        mongo::BSONObjBuilder builder;
        doc.writeTo(&builder);
        ASSERT_EQUALS(mongo::fromjson(outJson), doc.getObject());
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
        pun.appendString("na", "not");
        mongo::BSONObj outObj = doc.getObject();

        static const char outJson[] =
            "{"
            "  'xs': { 'x' : 'x', 'X' : 'X' },"
            "  'ys': { 'y' : 'y', 'Y' : 'Y', 'why' : ['not'] }"
            "}";
        ASSERT_EQUALS(mongo::fromjson(outJson), outObj);
    }

    TEST(Document, LifecycleConstructDefault) {
        // Verify the state of a newly created empty Document.
        mmb::Document doc;
        ASSERT_TRUE(doc.root().ok());
        ASSERT_TRUE(const_cast<const mmb::Document&>(doc).root().ok());
        ASSERT_TRUE(doc.root().isType(mongo::Object));
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
        ASSERT_TRUE(doc.root().isType(mongo::Object));
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
        ASSERT_TRUE(doc.root().isType(mongo::Object));
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
        ASSERT_TRUE(e1Child.isType(mongo::NumberInt));
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
        ASSERT_TRUE(e2Child.isType(mongo::String));
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
        ASSERT_TRUE(e3Child.isType(mongo::Bool));
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
        c.rename("C");
        mongo::BSONObj outObj = doc.getObject();
        static const char outJson[] =
            "{"
            "  'a' : { 'b' : { 'C' : { 'd' : 4 } } }"
            "}";
        ASSERT_EQUALS(mongo::fromjson(outJson), outObj);
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
        a.remove();

        static const char outJson[] =
            "{"
            "  'b' : 2, 'c' : 3"
            "}";
        mongo::BSONObj outObj = doc.getObject();
        ASSERT_EQUALS(mongo::fromjson(outJson), outObj);
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
        ASSERT_EQUALS(mongo::fromjson(outJson), outObj);
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
        ASSERT_EQUALS(mongo::NumberInt, a.getType());

        mmb::Element b = a.rightSibling();
        ASSERT_TRUE(b.ok());
        ASSERT_EQUALS("b", b.getFieldName());
        ASSERT_EQUALS(mongo::Array, b.getType());

        mmb::Element b0 = b[0];
        ASSERT_TRUE(b0.ok());
        ASSERT_EQUALS("0", b0.getFieldName());
        ASSERT_EQUALS(mongo::Object, b0.getType());
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
        ASSERT_EQUALS(mongo::NumberInt, a.getType());

        mmb::Element b = a.rightSibling();
        ASSERT_TRUE(b.ok());
        ASSERT_EQUALS("b", b.getFieldName());
        ASSERT_EQUALS(mongo::Array, b.getType());

        mmb::Element b0 = b[0];
        ASSERT_TRUE(b0.ok());
        ASSERT_EQUALS("ignored", b0.getFieldName());
        ASSERT_EQUALS(mongo::Object, b0.getType());
    }

} // namespace

