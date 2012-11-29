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

#include "mongo/bson/mutable/mutable_bson_algo.h"

#include "mongo/bson/mutable/mutable_bson_heap.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::Status;
    using namespace mongo::mutablebson;

    class DocumentTest : public mongo::unittest::Test {
    public:
        DocumentTest()
            : _heap()
            , _doc(&_heap) {}

        Document& doc() { return _doc; }

    private:
        BasicHeap _heap;
        Document _doc;
    };

    TEST_F(DocumentTest, FindInEmptyObject) {
        const SiblingIterator children = doc().root().children();
        ASSERT_TRUE(children.done());
        SiblingIterator next = findElementNamed(children, "X");
        ASSERT_TRUE(next.done());
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
        ASSERT_EQUALS(children.getRep(), next.getRep());
    }

    class OneChildTest : public DocumentTest {
        virtual void setUp() {
            ASSERT_EQUALS(Status::OK(), doc().root().appendBool("t", true));
        }
    };

    TEST_F(OneChildTest, FindNoMatch) {
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator next = findElementNamed(children, "f");
        ASSERT_TRUE(next.done());
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
    }

    TEST_F(OneChildTest, FindMatch) {
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator next = findElementNamed(children, "t");
        ASSERT_FALSE(next.done());
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
        ASSERT_EQUALS((*next).getFieldName(), "t");
        next = findElementNamed(++next, "t");
        ASSERT_TRUE(next.done());
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
    }

    class ManyChildrenTest : public DocumentTest {
        virtual void setUp() {
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("begin", "a"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("repeated_sparse", "b"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("repeated_dense", "c"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("repeated_dense", "d"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("middle", "e"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("repeated_sparse", "f"));
            ASSERT_EQUALS(Status::OK(), doc().root().appendString("end", "g"));
        }
    };

    TEST_F(ManyChildrenTest, FindAtStart) {
        static const char kName[] = "begin";
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator next = findElementNamed(children, kName);
        ASSERT_FALSE(next.done());
        ASSERT_EQUALS((*next).getFieldName(), kName);
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
        ASSERT_TRUE(findElementNamed(++next, kName).done());
    }

    TEST_F(ManyChildrenTest, FindInMiddle) {
        static const char kName[] = "middle";
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator next = findElementNamed(children, kName);
        ASSERT_FALSE(next.done());
        ASSERT_EQUALS((*next).getFieldName(), kName);
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
        ASSERT_TRUE(findElementNamed(++next, kName).done());
    }

    TEST_F(ManyChildrenTest, FindAtEnd) {
        static const char kName[] = "end";
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator next = findElementNamed(children, kName);
        ASSERT_FALSE(next.done());
        ASSERT_EQUALS((*next).getFieldName(), kName);
        ASSERT_EQUALS(children.getDocument(), next.getDocument());
        ASSERT_TRUE(findElementNamed(++next, kName).done());
    }

    TEST_F(ManyChildrenTest, FindRepeatedSparse) {
        static const char kName[] = "repeated_sparse";
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator first = findElementNamed(children, kName);
        ASSERT_FALSE(first.done());
        ASSERT_EQUALS((*first).getFieldName(), kName);
        ASSERT_EQUALS(children.getDocument(), first.getDocument());
        SiblingIterator second = findElementNamed(++SiblingIterator(first), kName);
        ASSERT_FALSE(second.done());
        ASSERT_EQUALS(first.getDocument(), second.getDocument());
        ASSERT_NOT_EQUALS(first.getRep(), second.getRep());
        SiblingIterator none = findElementNamed(++SiblingIterator(second), kName);
        ASSERT_TRUE(none.done());
    }

    TEST_F(ManyChildrenTest, FindRepeatedDense) {
        static const char kName[] = "repeated_dense";
        const SiblingIterator children = doc().root().children();
        ASSERT_FALSE(children.done());
        SiblingIterator first = findElementNamed(children, kName);
        ASSERT_FALSE(first.done());
        ASSERT_EQUALS((*first).getFieldName(), kName);
        ASSERT_EQUALS(children.getDocument(), first.getDocument());
        SiblingIterator second = findElementNamed(++SiblingIterator(first), kName);
        ASSERT_FALSE(second.done());
        ASSERT_EQUALS(first.getDocument(), second.getDocument());
        ASSERT_NOT_EQUALS(first.getRep(), second.getRep());
        SiblingIterator none = findElementNamed(++SiblingIterator(second), kName);
        ASSERT_TRUE(none.done());
    }

    TEST_F(ManyChildrenTest, FindDoesNotSearchWithinChildren) {
        static const char kName[] = "in_child";
        SiblingIterator found_before_add = findElementNamed(doc().root().children(), kName);
        ASSERT_TRUE(found_before_add.done());
        Element subdoc = doc().makeObjElement("child");
        ASSERT_EQUALS(Status::OK(), doc().root().addChild(subdoc));
        ASSERT_EQUALS(Status::OK(), subdoc.appendBool(kName, true));
        SiblingIterator found_after_add = findElementNamed(doc().root().children(), kName);
        ASSERT_TRUE(found_after_add.done());
    }

} // namespace
