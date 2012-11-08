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

#include <iostream>

#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/bson/mutable/mutable_bson.h"
#include "mongo/bson/mutable/mutable_bson_internal.h"
#include "mongo/bson/mutable/mutable_bson_heap.h"
#include "mongo/bson/mutable/mutable_bson_builder.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/json.h"

namespace {

#define EMPTY_REP  ((uint32_t)-1)
#define __TRACE__ __FILE__ << ":" << __FUNCTION__ << " [" << __LINE__ << "]"

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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());

        ASSERT_EQUALS("e0", e0.fieldName());
        ASSERT_EQUALS("e1", e0.leftChild().fieldName());
        ASSERT_EQUALS("e2", e0.rightChild().fieldName());
        ASSERT_EQUALS("e0", e1.parent().fieldName());
        ASSERT_EQUALS("e0", e2.parent().fieldName());
        ASSERT_EQUALS("e2", e1.rightSibling().fieldName());
        ASSERT_EQUALS("e1", e2.leftSibling().fieldName());
        ASSERT_EQUALS("e3", e2.leftChild().fieldName());
        ASSERT_EQUALS("e3", e2.rightChild().fieldName());

        ASSERT_EQUALS("e2", e3.parent().fieldName());
        ASSERT_EQUALS("e4", e3.leftChild().fieldName());
        ASSERT_EQUALS("e5", e3.rightChild().fieldName());
        ASSERT_EQUALS("e4", e5.leftSibling().fieldName());
        ASSERT_EQUALS("e5", e4.rightSibling().fieldName());
        ASSERT_EQUALS("e3", e4.parent().fieldName());
        ASSERT_EQUALS("e3", e5.parent().fieldName());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addSiblingAfter(e4), mongo::Status::OK());

        ASSERT_EQUALS("e4", e3.rightSibling().fieldName());
        ASSERT_EQUALS("e3", e4.leftSibling().fieldName());
        ASSERT_EQUALS("e2", e4.parent().fieldName());
        ASSERT_EQUALS("e3", e2.leftChild().fieldName());
        ASSERT_EQUALS("e4", e2.rightChild().fieldName());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");
        mongo::mutablebson::Element e6 = ctx.makeObjElement("e6");
        mongo::mutablebson::Element e7 = ctx.makeObjElement("e7");

        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingAfter(e6), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingBefore(e7), mongo::Status::OK());

        ASSERT_EQUALS("e5", e7.rightSibling().fieldName());
        ASSERT_EQUALS("e7", e5.leftSibling().fieldName());
        ASSERT_EQUALS("e6", e5.rightSibling().fieldName());
        ASSERT_EQUALS("e5", e6.leftSibling().fieldName());

        ASSERT_EQUALS("e3", e5.parent().fieldName());
        ASSERT_EQUALS("e3", e6.parent().fieldName());
        ASSERT_EQUALS("e3", e7.parent().fieldName());
        ASSERT_EQUALS("e7", e3.leftChild().fieldName());
        ASSERT_EQUALS("e6", e3.rightChild().fieldName());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");
        mongo::mutablebson::Element e6 = ctx.makeObjElement("e6");
        mongo::mutablebson::Element e8 = ctx.makeObjElement("e8");
        mongo::mutablebson::Element e9 = ctx.makeObjElement("e9");
        mongo::mutablebson::Element e10 = ctx.makeObjElement("e10");

        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e6), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e9), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e10), mongo::Status::OK());
        ASSERT_EQUALS(e5.addChild(e8), mongo::Status::OK());

        ASSERT_EQUALS("e8", e9.parent().fieldName());
        ASSERT_EQUALS("e8", e10.parent().fieldName());
        ASSERT_EQUALS("e9", e8.leftChild().fieldName());
        ASSERT_EQUALS("e10", e8.rightChild().fieldName());
        ASSERT_EQUALS("e9", e10.leftSibling().fieldName());
        ASSERT_EQUALS("e10", e9.rightSibling().fieldName());
        ASSERT_EQUALS("e5", e8.parent().fieldName());
        ASSERT_EQUALS("e8", e5.leftChild().fieldName());
        ASSERT_EQUALS("e8", e5.rightChild().fieldName());
    }

    TEST(TopologyBuilding, RemoveLeafNode) {
        /*
                       [ e0 ]                [ e0 ]
                        /   \       =>          \
                       /     \                   \
                   [ e1 ]   [ e2 ]              [ e2 ]
        */
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e1.remove(), mongo::Status::OK());

        ASSERT_EQUALS("e2", e0.leftChild().fieldName());
        ASSERT_EQUALS("e2", e0.rightChild().fieldName());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");
        mongo::mutablebson::Element e6 = ctx.makeObjElement("e6");
        mongo::mutablebson::Element e8 = ctx.makeObjElement("e8");
        mongo::mutablebson::Element e9 = ctx.makeObjElement("e9");
        mongo::mutablebson::Element e10 = ctx.makeObjElement("e10");

        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e5.addSiblingAfter(e6), mongo::Status::OK());

        ASSERT_EQUALS(e8.addChild(e9), mongo::Status::OK());
        ASSERT_EQUALS(e8.addChild(e10), mongo::Status::OK());
        ASSERT_EQUALS(e5.addChild(e8), mongo::Status::OK());
        ASSERT_EQUALS(e5.remove(), mongo::Status::OK());

        ASSERT_EQUALS("e3", e4.parent().fieldName());
        ASSERT_EQUALS("e3", e6.parent().fieldName());
        ASSERT_EQUALS("e4", e3.leftChild().fieldName());
        ASSERT_EQUALS("e6", e3.rightChild().fieldName());
    }

    TEST(TopologyBuilding, RenameNode) {
        /*

                       [ e0 ]                  [ f0 ]
                        /  \          =>        /  \
                       /    \                  /    \
                   [ e1 ]..[ e2 ]          [ e1 ]..[ e2 ]
        */

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e0.rename("f0"), mongo::Status::OK());

        ASSERT_EQUALS("f0", e0.fieldName());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());

        ASSERT_EQUALS("e0", e0.fieldName());
        ASSERT_EQUALS("e1", e0.leftChild().fieldName());
        ASSERT_EQUALS("e2", e0.rightChild().fieldName());
        ASSERT_EQUALS("e0", e1.parent().fieldName());
        ASSERT_EQUALS("e0", e2.parent().fieldName());
        ASSERT_EQUALS("e2", e1.rightSibling().fieldName());
        ASSERT_EQUALS("e1", e2.leftSibling().fieldName());
        ASSERT_EQUALS("e3", e2.leftChild().fieldName());
        ASSERT_EQUALS("e3", e2.rightChild().fieldName());
        ASSERT_EQUALS("e4", e3.leftChild().fieldName());
        ASSERT_EQUALS("e5", e3.rightChild().fieldName());
        ASSERT_EQUALS("e5", e4.rightSibling().fieldName());
        ASSERT_EQUALS("e4", e5.leftSibling().fieldName());
        ASSERT_EQUALS(e3.move(e0), mongo::Status::OK());

        ASSERT_EQUALS("e0", e3.parent().fieldName());
        ASSERT_EQUALS("e1", e0.leftChild().fieldName());
        ASSERT_EQUALS("e3", e0.rightChild().fieldName());
        ASSERT_EQUALS("e3", e2.rightSibling().fieldName());
        ASSERT_EQUALS("e2", e3.leftSibling().fieldName());
        ASSERT_EQUALS("e4", e3.leftChild().fieldName());
        ASSERT_EQUALS("e5", e3.rightChild().fieldName());
    }


    TEST(Iterators, SubtreeIterator) {
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());

        mongo::mutablebson::SubtreeIterator it(e0);

        ASSERT_EQUALS(it.done(), false);
        ASSERT_EQUALS("e0", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e1", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e2", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e3", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e4", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e5", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), true);
    }

    TEST(Iterators, FieldnameIterator) {
        /* 
                           [ e0 ]
                            /   \
                           /     \
                       [ e1 ]..[ e2 ]
                        /       /
                       /       /
                   [ e6 ]  [ e3 ]
                            /   \
                           /     \
                       [ e4 ]..[ e5 ]
        */

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e3 = ctx.makeObjElement("e3");
        mongo::mutablebson::Element e4 = ctx.makeObjElement("e4");
        mongo::mutablebson::Element e5 = ctx.makeObjElement("e5");
        mongo::mutablebson::Element e6 = ctx.makeObjElement("e3");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e2), mongo::Status::OK());
        ASSERT_EQUALS(e2.addChild(e3), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e4), mongo::Status::OK());
        ASSERT_EQUALS(e3.addChild(e5), mongo::Status::OK());
        ASSERT_EQUALS(e1.addChild(e6), mongo::Status::OK());

        mongo::mutablebson::FieldNameFilter filter("e3");

        ASSERT_EQUALS(filter.match(e0), false);
        ASSERT_EQUALS(filter.match(e1), false);
        ASSERT_EQUALS(filter.match(e2), false);
        ASSERT_EQUALS(filter.match(e3), true);
        ASSERT_EQUALS(filter.match(e4), false);
        ASSERT_EQUALS(filter.match(e5), false);
        ASSERT_EQUALS(filter.match(e6), true);

        mongo::mutablebson::FilterIterator it(e0, &filter);

        ASSERT_EQUALS(it.done(), false);
        ASSERT_EQUALS("e3", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e3", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), true);
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e1 = ctx.makeArrayElement("a");
        mongo::mutablebson::Element e2 = ctx.makeIntElement("", 10);
        ASSERT_EQUALS(10, e2.Int());
        mongo::mutablebson::Element e3 = ctx.makeIntElement("", 20);
        ASSERT_EQUALS(20, e3.Int());
        mongo::mutablebson::Element e4 = ctx.makeIntElement("", 30);
        ASSERT_EQUALS(30, e4.Int());
        mongo::mutablebson::Element e5 = ctx.makeIntElement("", 40);
        ASSERT_EQUALS(40, e5.Int());
        mongo::mutablebson::Element e6 = ctx.makeIntElement("", 5);
        ASSERT_EQUALS(5, e6.Int());
        mongo::mutablebson::Element e7 = ctx.makeIntElement("", 0);
        ASSERT_EQUALS(0, e7.Int());
    
        ASSERT_EQUALS(e1.pushBack(e2), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e3), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e4), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushBack(e5), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushFront(e6), mongo::Status::OK());
        ASSERT_EQUALS(e1.pushFront(e7), mongo::Status::OK());

        uint32_t n;
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(6, (int)n);

        mongo::mutablebson::Element e(&ctx, EMPTY_REP);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.Int());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.Int());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());
        ASSERT_EQUALS(e1.get(4, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.Int());
        ASSERT_EQUALS(e1.get(5, &e), mongo::Status::OK());
        ASSERT_EQUALS(40, e.Int());

        ASSERT_EQUALS(e1.peekBack(&e), mongo::Status::OK());
        ASSERT_EQUALS(40, e.Int());
        ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(5, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.Int());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.Int());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());
        ASSERT_EQUALS(e1.get(4, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.Int());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(0, e.Int());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(4, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.Int());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());
        ASSERT_EQUALS(e1.get(3, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.Int());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(5, e.Int());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(3, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());
        ASSERT_EQUALS(e1.get(2, &e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.Int());

        ASSERT_EQUALS(e1.peekBack(&e), mongo::Status::OK());
        ASSERT_EQUALS(30, e.Int());
        ASSERT_EQUALS(e1.popBack(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(2, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.get(1, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());

        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(10, e.Int());
        ASSERT_EQUALS(e1.popFront(), mongo::Status::OK());
        ASSERT_EQUALS(e1.arraySize(&n), mongo::Status::OK());
        ASSERT_EQUALS(1, (int)n);
        ASSERT_EQUALS(e1.get(0, &e), mongo::Status::OK());
        ASSERT_EQUALS(20, e.Int());
        
        mongo::mutablebson::Element e8 = ctx.makeIntElement("", 100);
        ASSERT_EQUALS(100, e8.Int());
        ASSERT_EQUALS(e1.set(0, e8), mongo::Status::OK());
        ASSERT_EQUALS(e1.peekFront(&e), mongo::Status::OK());
        ASSERT_EQUALS(100, e.Int());
    }


    TEST(Lookup, ByName) {
        /*
                       [ e0 ]
                        /   \
                       /     \
                   [ e1 ]..[ e2 ]
                     /
                    /
                 [ e2 ]
        */

        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element e0 = ctx.makeObjElement("e0");
        mongo::mutablebson::Element e1 = ctx.makeObjElement("e1");
        mongo::mutablebson::Element e0e2 = ctx.makeObjElement("e2");
        mongo::mutablebson::Element e1e2 = ctx.makeObjElement("e2");

        ASSERT_EQUALS(e0.addChild(e1), mongo::Status::OK());
        ASSERT_EQUALS(e0.addChild(e0e2), mongo::Status::OK());
        ASSERT_EQUALS(e1.addChild(e1e2), mongo::Status::OK());

        ASSERT_EQUALS("e0", e0e2.parent().fieldName());
        ASSERT_EQUALS("e1", e1e2.parent().fieldName());

        mongo::mutablebson::FilterIterator it = e0.find("e2");
        ASSERT_EQUALS(it.done(), false);
        ASSERT_EQUALS("e2", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS("e1", mongo::mutablebson::Element(&ctx, it.getRep()).parent().fieldName());

        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("e2", mongo::mutablebson::Element(&ctx, it.getRep()).fieldName());
        ASSERT_EQUALS("e0", mongo::mutablebson::Element(&ctx, it.getRep()).parent().fieldName());

        ASSERT_EQUALS((++it).done(), true);
    }

    TEST(Element, setters) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element t0 = ctx.makeNullElement("t0");

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

        t0.setOIDValue("012345678901");
        ASSERT_EQUALS(mongo::jstOID, t0.type());

        t0.setRegexValue("[a-zA-Z]?");
        ASSERT_EQUALS(mongo::RegEx, t0.type());

        t0.setStringValue("foo bar baz");
        ASSERT_EQUALS(mongo::String, t0.type());
    }

    TEST(TimestampType, createElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element t0 = ctx.makeTSElement("t0", mongo::OpTime());
        ASSERT(mongo::OpTime() == t0.getTSValue());

        mongo::mutablebson::Element t1 = ctx.makeTSElement("t1", mongo::OpTime(123, 456));
        ASSERT(mongo::OpTime(123, 456) == t1.getTSValue());
    }

    TEST(TimestampType, setElement) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element t0 = ctx.makeTSElement("t0", mongo::OpTime());
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
        mongo::mutablebson::Context ctx(&myHeap);

        mongo::mutablebson::Element t0 = ctx.makeObjElement("e0");
        t0.appendTS("a timestamp field", mongo::OpTime(1352151971, 471));

        mongo::mutablebson::FilterIterator it = t0.find("a timestamp field");
        ASSERT_EQUALS(it.done(), false);
        ASSERT(mongo::OpTime(1352151971, 471) ==
            mongo::mutablebson::Element(&ctx, it.getRep()).getTSValue());
    }
} // unnamed namespace
