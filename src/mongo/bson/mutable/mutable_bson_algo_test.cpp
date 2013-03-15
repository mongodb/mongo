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

#include "mongo/bson/mutable/algorithm.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::Status;
    using namespace mongo::mutablebson;

    class DocumentTest : public mongo::unittest::Test {
    public:
        DocumentTest()
            : _doc() {}

        Document& doc() { return _doc; }

    private:
        Document _doc;
    };

    TEST_F(DocumentTest, FindInEmptyObject) {
        Element leftChild = doc().root().leftChild();
        ASSERT_FALSE(leftChild.ok());
        Element found = findElementNamed(leftChild, "X");
        ASSERT_FALSE(found.ok());
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
        ASSERT_EQUALS(leftChild.getIdx(), found.getIdx());
    }

    class OneChildTest : public DocumentTest {
        virtual void setUp() {
            ASSERT_EQUALS(Status::OK(), doc().root().appendBool("t", true));
        }
    };

    TEST_F(OneChildTest, FindNoMatch) {
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element found = findElementNamed(leftChild, "f");
        ASSERT_FALSE(found.ok());
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
    }

    TEST_F(OneChildTest, FindMatch) {
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element found = findElementNamed(leftChild, "t");
        ASSERT_TRUE(found.ok());
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
        ASSERT_EQUALS(found.getFieldName(), "t");
        found = findElementNamed(found.rightSibling(), "t");
        ASSERT_FALSE(found.ok());
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
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
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element found = findElementNamed(leftChild, kName);
        ASSERT_TRUE(found.ok());
        ASSERT_EQUALS(found.getFieldName(), kName);
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
        ASSERT_FALSE(findElementNamed(found.rightSibling(), kName).ok());
    }

    TEST_F(ManyChildrenTest, FindInMiddle) {
        static const char kName[] = "middle";
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element found = findElementNamed(leftChild, kName);
        ASSERT_TRUE(found.ok());
        ASSERT_EQUALS(found.getFieldName(), kName);
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
        ASSERT_FALSE(findElementNamed(found.rightSibling(), kName).ok());
    }

    TEST_F(ManyChildrenTest, FindAtEnd) {
        static const char kName[] = "end";
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element found = findElementNamed(leftChild, kName);
        ASSERT_TRUE(found.ok());
        ASSERT_EQUALS(found.getFieldName(), kName);
        ASSERT_EQUALS(&leftChild.getDocument(), &found.getDocument());
        ASSERT_FALSE(findElementNamed(found.rightSibling(), kName).ok());
    }

    TEST_F(ManyChildrenTest, FindRepeatedSparse) {
        static const char kName[] = "repeated_sparse";
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element first = findElementNamed(leftChild, kName);
        ASSERT_TRUE(first.ok());
        ASSERT_EQUALS(first.getFieldName(), kName);
        ASSERT_EQUALS(&leftChild.getDocument(), &first.getDocument());
        Element second = findElementNamed(first.rightSibling(), kName);
        ASSERT_TRUE(second.ok());
        ASSERT_EQUALS(&first.getDocument(), &second.getDocument());
        ASSERT_NOT_EQUALS(first.getIdx(), second.getIdx());
        Element none = findElementNamed(second.rightSibling(), kName);
        ASSERT_FALSE(none.ok());
    }

    TEST_F(ManyChildrenTest, FindRepeatedDense) {
        static const char kName[] = "repeated_dense";
        Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        Element first = findElementNamed(leftChild, kName);
        ASSERT_TRUE(first.ok());
        ASSERT_EQUALS(first.getFieldName(), kName);
        ASSERT_EQUALS(&leftChild.getDocument(), &first.getDocument());
        Element second = findElementNamed(first.rightSibling(), kName);
        ASSERT_TRUE(second.ok());
        ASSERT_EQUALS(&first.getDocument(), &second.getDocument());
        ASSERT_NOT_EQUALS(first.getIdx(), second.getIdx());
        Element none = findElementNamed(second.rightSibling(), kName);
        ASSERT_FALSE(none.ok());
    }

    TEST_F(ManyChildrenTest, FindDoesNotSearchWithinChildren) {
        static const char kName[] = "in_child";
        Element found_before_add = findElementNamed(doc().root().leftChild(), kName);
        ASSERT_FALSE(found_before_add.ok());
        Element subdoc = doc().makeElementObject("child");
        ASSERT_EQUALS(Status::OK(), doc().root().pushBack(subdoc));
        ASSERT_EQUALS(Status::OK(), subdoc.appendBool(kName, true));
        Element found_after_add = findElementNamed(doc().root().leftChild(), kName);
        ASSERT_FALSE(found_after_add.ok());
    }

} // namespace
