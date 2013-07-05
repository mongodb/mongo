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
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
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

    TEST_F(ManyChildrenTest, getNthSibling) {
        const Element leftChild = doc().root().leftChild();
        ASSERT_TRUE(leftChild.ok());
        const Element rightChild = doc().root().rightChild();
        ASSERT_TRUE(rightChild.ok());

        // Check that moving zero is a no-op
        Element zeroAway = getNthSibling(leftChild, 0);
        ASSERT_TRUE(zeroAway.ok());
        ASSERT_EQUALS(leftChild, zeroAway);
        zeroAway = getNthSibling(rightChild, 0);
        ASSERT_TRUE(zeroAway.ok());
        ASSERT_EQUALS(rightChild, zeroAway);

        // Check that moving left of leftmost gets a not-ok element.
        Element badLeft = getNthSibling(leftChild, -1);
        ASSERT_FALSE(badLeft.ok());

        // Check that moving right of rightmost gets a non-ok element.
        Element badRight = getNthSibling(rightChild, 1);
        ASSERT_FALSE(badRight.ok());

        // Check that the moving one right from leftmost gets us the expected element.
        Element target = leftChild.rightSibling();
        ASSERT_TRUE(target.ok());
        Element query = getNthSibling(leftChild, 1);
        ASSERT_TRUE(target.ok());
        ASSERT_EQUALS(target, query);

        // And the same from the other side
        target = rightChild.leftSibling();
        ASSERT_TRUE(target.ok());
        query = getNthSibling(rightChild, -1);
        ASSERT_TRUE(target.ok());
        ASSERT_EQUALS(target, query);

        // Ensure that walking more chidren than we have gets us past the end
        const int children = countChildren(doc().root());
        query = getNthSibling(leftChild, children);
        ASSERT_FALSE(query.ok());
        query = getNthSibling(rightChild, -children);
        ASSERT_FALSE(query.ok());

        // Ensure that walking all the children in either direction gets
        // us to the other right/left child.
        query = getNthSibling(leftChild, children - 1);
        ASSERT_TRUE(query.ok());
        ASSERT_EQUALS(rightChild, query);
        query = getNthSibling(rightChild, -(children - 1));
        ASSERT_TRUE(query.ok());
        ASSERT_EQUALS(leftChild, query);
    }

    class CountTest : public DocumentTest {
        virtual void setUp() {
            Element root = doc().root();

            ASSERT_OK(root.appendInt("leaf", 0));

            Element one = doc().makeElementObject("oneChild");
            ASSERT_TRUE(one.ok());
            ASSERT_OK(one.appendInt("one", 1));
            ASSERT_OK(root.pushBack(one));

            Element threeChildren = doc().makeElementObject("threeChildren");
            ASSERT_TRUE(one.ok());
            ASSERT_OK(threeChildren.appendInt("one", 1));
            ASSERT_OK(threeChildren.appendInt("two", 2));
            ASSERT_OK(threeChildren.appendInt("three", 3));
            ASSERT_OK(root.pushBack(threeChildren));
        }
    };

    TEST_F(CountTest, EmptyDocument) {
        // Doesn't use the fixture but belongs in the same group of tests.
        Document doc;
        ASSERT_EQUALS(countChildren(doc.root()), 0u);
    }

    TEST_F(CountTest, EmptyElement) {
        Element leaf = findFirstChildNamed(doc().root(), "leaf");
        ASSERT_TRUE(leaf.ok());
        ASSERT_EQUALS(countChildren(leaf), 0u);
    }

    TEST_F(CountTest, OneChildElement) {
        Element oneChild = findFirstChildNamed(doc().root(), "oneChild");
        ASSERT_TRUE(oneChild.ok());
        ASSERT_EQUALS(countChildren(oneChild), 1u);
    }

    TEST_F(CountTest, ManyChildren) {
        Element threeChildren = findFirstChildNamed(doc().root(), "threeChildren");
        ASSERT_TRUE(threeChildren.ok());
        ASSERT_EQUALS(countChildren(threeChildren), 3u);
    }

    TEST_F(CountTest, CountSiblingsNone) {
        ConstElement current = findFirstChildNamed(doc().root(), "oneChild");
        ASSERT_TRUE(current.ok());

        current = current.leftChild();
        ASSERT_TRUE(current.ok());

        ASSERT_EQUALS(0U, countSiblingsLeft(current));
        ASSERT_EQUALS(0U, countSiblingsRight(current));
    }

    TEST_F(CountTest, CountSiblingsMany) {
        ConstElement current = findFirstChildNamed(doc().root(), "threeChildren");
        ASSERT_TRUE(current.ok());

        current = current.leftChild();
        ASSERT_TRUE(current.ok());

        ASSERT_EQUALS(0U, countSiblingsLeft(current));
        ASSERT_EQUALS(2U, countSiblingsRight(current));

        current = current.rightSibling();
        ASSERT_TRUE(current.ok());
        ASSERT_EQUALS(1U, countSiblingsLeft(current));
        ASSERT_EQUALS(1U, countSiblingsRight(current));

        current = current.rightSibling();
        ASSERT_TRUE(current.ok());
        ASSERT_EQUALS(2U, countSiblingsLeft(current));
        ASSERT_EQUALS(0U, countSiblingsRight(current));

        current = current.rightSibling();
        ASSERT_FALSE(current.ok());
    }

    TEST(DeduplicateTest, ManyDuplicates) {
        Document doc(mongo::fromjson("{ x : [ 1, 2, 2, 3, 3, 3, 4, 4, 4 ] }"));
        deduplicateChildren(doc.root().leftChild(), woEqual(false));
        ASSERT_TRUE(checkDoc(doc, mongo::fromjson("{x : [ 1, 2, 3, 4 ]}")));
    }

} // namespace
