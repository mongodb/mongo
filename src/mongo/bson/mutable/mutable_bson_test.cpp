/* Copyright 2010 10gen Inc.
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

#include "mongo/bson/mutable/mutable_bson.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/mutable_bson_algo.h"
#include "mongo/bson/mutable/mutable_bson_builder.h"
#include "mongo/bson/mutable/mutable_bson_heap.h"
#include "mongo/bson/mutable/mutable_bson_internal.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

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

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e0 = doc.makeObjElement("e0");
        mongo::mutablebson::Element e1 = doc.makeObjElement("e1");
        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");
        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");
        mongo::mutablebson::Element e5 = doc.makeObjElement("e5");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());

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

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e0 = doc.makeObjElement("e0");
        mongo::mutablebson::Element e1 = doc.makeObjElement("e1");
        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");
        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addSiblingAfter(e4), mongo::Status::OK());

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

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");
        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");
        mongo::mutablebson::Element e5 = doc.makeObjElement("e5");
        mongo::mutablebson::Element e6 = doc.makeObjElement("e6");
        mongo::mutablebson::Element e7 = doc.makeObjElement("e7");

        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingAfter(e6), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingBefore(e7), mongo::Status::OK());

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
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");
        mongo::mutablebson::Element e5 = doc.makeObjElement("e5");
        mongo::mutablebson::Element e6 = doc.makeObjElement("e6");
        mongo::mutablebson::Element e8 = doc.makeObjElement("e8");
        mongo::mutablebson::Element e9 = doc.makeObjElement("e9");
        mongo::mutablebson::Element e10 = doc.makeObjElement("e10");

        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e6), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e9), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e10), mongo::Status::OK());
        ASSERT_EQUALS(e5.addChild(e8), mongo::Status::OK());

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
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e0 = doc.makeObjElement("e0");
        mongo::mutablebson::Element e1 = doc.makeObjElement("e1");
        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
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
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");
        mongo::mutablebson::Element e5 = doc.makeObjElement("e5");
        mongo::mutablebson::Element e6 = doc.makeObjElement("e6");
        mongo::mutablebson::Element e8 = doc.makeObjElement("e8");
        mongo::mutablebson::Element e9 = doc.makeObjElement("e9");
        mongo::mutablebson::Element e10 = doc.makeObjElement("e10");

        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingAfter(e6), mongo::Status::OK());

        ASSERT_EQUALS(e8.addChild(e9), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e10), mongo::Status::OK());
        ASSERT_EQUALS(e5.addChild(e8), mongo::Status::OK());
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

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e0 = doc.makeObjElement("e0");
        mongo::mutablebson::Element e1 = doc.makeObjElement("e1");
        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
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

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e0 = doc.makeObjElement("e0");
        mongo::mutablebson::Element e1 = doc.makeObjElement("e1");
        mongo::mutablebson::Element e2 = doc.makeObjElement("e2");
        mongo::mutablebson::Element e3 = doc.makeObjElement("e3");
        mongo::mutablebson::Element e4 = doc.makeObjElement("e4");
        mongo::mutablebson::Element e5 = doc.makeObjElement("e5");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());

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
        ASSERT_EQUALS(e3.move(e0), mongo::Status::OK());

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
        */
  
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element e1 = doc.makeArrayElement("a");
        mongo::mutablebson::Element e2 = doc.makeIntElement("", 10);
        ASSERT_EQUALS(10, e2.getIntValue());
        mongo::mutablebson::Element e3 = doc.makeIntElement("", 20);
        ASSERT_EQUALS(20, e3.getIntValue());
        mongo::mutablebson::Element e4 = doc.makeIntElement("", 30);
        ASSERT_EQUALS(30, e4.getIntValue());
        mongo::mutablebson::Element e5 = doc.makeIntElement("", 40);
        ASSERT_EQUALS(40, e5.getIntValue());
        mongo::mutablebson::Element e6 = doc.makeIntElement("", 5);
        ASSERT_EQUALS(5, e6.getIntValue());
        mongo::mutablebson::Element e7 = doc.makeIntElement("", 0);
        ASSERT_EQUALS(0, e7.getIntValue());
    
        ASSERT_EQUALS(e1.pushBack(e2), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e3), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e4), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e5), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushFront(e6), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushFront(e7), mongo::Status::OK());

        uint32_t n;
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(6, (int)n);

        mongo::mutablebson::Element e(&doc, mongo::mutablebson::EMPTY_REP);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.getIntValue());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.getIntValue());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());
        ASSERT_EQUALS(e1.get(4, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.getIntValue());
        ASSERT_EQUALS(e1.get(5, &e), mongo::Status::OK());
        ASSERT_EQUALS(40, e.getIntValue());

        ASSERT_EQUALS(e1.peekBack(&e), mongo::Status::OK());
        ASSERT_EQUALS(40, e.getIntValue());
        ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(5, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.getIntValue());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.getIntValue());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());
        ASSERT_EQUALS(e1.get(4, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.getIntValue());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.getIntValue());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(4, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.getIntValue());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.getIntValue());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.getIntValue());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(3, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.getIntValue());

        ASSERT_EQUALS(e1.peekBack(&e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.getIntValue());
        ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(2, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.getIntValue());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(1, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.getIntValue());
        
        mongo::mutablebson::Element e8 = doc.makeIntElement("", 100);
        ASSERT_EQUALS(100, e8.getIntValue());
        ASSERT_EQUALS(e1.set(0, e8), mongo::Status::OK());
        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(100, e.getIntValue());
    }

    TEST(Element, setters) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeNullElement("t0");

        t0.setBoolValue(true);
        ASSERT_EQUALS(mongo::Bool, t0.type());

        t0.setIntValue(12345);
        ASSERT_EQUALS(mongo::NumberInt, t0.type());

        t0.setLongValue(12345LL);
        ASSERT_EQUALS(mongo::NumberLong, t0.type());

        t0.setTSValue(mongo::OpTime());
        ASSERT_EQUALS(mongo::Timestamp, t0.type());

        t0.setDateValue(12345LL);
        ASSERT_EQUALS(mongo::Date, t0.type());

        t0.setDoubleValue(123.45);
        ASSERT_EQUALS(mongo::NumberDouble, t0.type());

        t0.setOIDValue(mongo::OID("47cc67093475061e3d95369d"));
        ASSERT_EQUALS(mongo::jstOID, t0.type());

        t0.setRegexValue("[a-zA-Z]?");
        ASSERT_EQUALS(mongo::RegEx, t0.type());

        t0.setStringValue("foo bar baz");
        ASSERT_EQUALS(mongo::String, t0.type());
    }

    TEST(TimestampType, createElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeTSElement("t0", mongo::OpTime());
        ASSERT(mongo::OpTime() == t0.getTSValue());

        mongo::mutablebson::Element t1 = doc.makeTSElement("t1", mongo::OpTime(123, 456));
        ASSERT(mongo::OpTime(123, 456) == t1.getTSValue());
    }

    TEST(TimestampType, setElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeTSElement("t0", mongo::OpTime());
        t0.setTSValue(mongo::OpTime(123, 456));
        ASSERT(mongo::OpTime(123, 456) == t0.getTSValue());

        // Try setting to other types and back to OpTime
        t0.setLongValue(1234567890);
        ASSERT_EQUALS(1234567890LL, t0.getLongValue());
        t0.setTSValue(mongo::OpTime(789, 321));
        ASSERT(mongo::OpTime(789, 321) == t0.getTSValue());

        t0.setStringValue("foo bar baz");
        ASSERT_EQUALS(0, strcmp("foo bar baz", t0.getStringValue()));
        t0.setTSValue(mongo::OpTime(9876, 5432));
        ASSERT(mongo::OpTime(9876, 5432) == t0.getTSValue());
    }

    TEST(TimestampType, appendElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeObjElement("e0");
        t0.appendTS("a timestamp field", mongo::OpTime(1352151971, 471));

        mongo::mutablebson::SiblingIterator it =
            mongo::mutablebson::findFirstChildNamed(t0, "a timestamp field");
        ASSERT_EQUALS(it.done(), false);
        ASSERT(mongo::OpTime(1352151971, 471) ==
            mongo::mutablebson::Element(&doc, it.getRep()).getTSValue());
    }

    TEST(SafeNumType, createElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeSafeNumElement("t0", mongo::SafeNum(123.456));
        ASSERT_EQUALS(mongo::SafeNum(123.456), t0.getSafeNumValue());
    }

    // Try getting SafeNums from different types.
    TEST(SafeNumType, getSafeNum) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeIntElement("t0", 1234567890);
        ASSERT_EQUALS(1234567890, t0.getIntValue());
        mongo::SafeNum num = t0.getSafeNumValue();
        ASSERT_EQUALS(num, 1234567890);

        t0.setLongValue(1234567890LL);
        ASSERT_EQUALS(1234567890LL, t0.getLongValue());
        num = t0.getSafeNumValue();
        ASSERT_EQUALS(num, 1234567890LL);

        t0.setDoubleValue(123.456789);
        ASSERT_EQUALS(123.456789, t0.getDoubleValue());
        num = t0.getSafeNumValue();
        ASSERT_EQUALS(num, 123.456789);
    }

    TEST(SafeNumType, setSafeNum) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeSafeNumElement("t0", mongo::SafeNum(123456));
        t0.setSafeNumValue(mongo::SafeNum(654321));
        ASSERT_EQUALS(mongo::SafeNum(654321), t0.getSafeNumValue());

        // Try setting to other types and back to SafeNum
        t0.setLongValue(1234567890);
        ASSERT_EQUALS(1234567890LL, t0.getLongValue());
        t0.setSafeNumValue(mongo::SafeNum(1234567890));
        ASSERT_EQUALS(mongo::SafeNum(1234567890), t0.getSafeNumValue());

        t0.setStringValue("foo bar baz");
        ASSERT_EQUALS("foo bar baz", std::string(t0.getStringValue()));
        t0.setSafeNumValue(mongo::SafeNum(12345));
        ASSERT_EQUALS(mongo::SafeNum(12345), t0.getSafeNumValue());
    }

    TEST(SafeNumType, appendElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        mongo::mutablebson::Element t0 = doc.makeObjElement("e0");
        t0.appendSafeNum("a timestamp field", mongo::SafeNum(1352151971LL));

        mongo::mutablebson::SiblingIterator it = findFirstChildNamed(t0, "a timestamp field");
        ASSERT_EQUALS(it.done(), false);
        ASSERT_EQUALS(mongo::SafeNum(1352151971LL),
            mongo::mutablebson::Element(&doc, it.getRep()).getSafeNumValue());
    }

    TEST(OIDType, getOidValue) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);
        mongo::mutablebson::Element t0 = doc.makeObjElement("e0");
        const mongo::OID generated = mongo::OID::gen();
        t0.appendOID("myOid", generated);
        mongo::mutablebson::SiblingIterator it = findFirstChildNamed(t0, "myOid");
        const mongo::OID recovered = mongo::OID((*it).getOIDValue());
        ASSERT_EQUALS(generated, recovered);
    }

    TEST(OIDType, nullOID) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);
        mongo::mutablebson::Element t0 = doc.makeObjElement("e0");
        const mongo::OID withNull("50a9c82263e413ad0028faad");
        t0.appendOID("myOid", withNull);
        mongo::mutablebson::SiblingIterator it = findFirstChildNamed(t0, "myOid");
        const mongo::OID recovered = mongo::OID((*it).getOIDValue());
        ASSERT_EQUALS(withNull, recovered);
    }

} // unnamed namespace
