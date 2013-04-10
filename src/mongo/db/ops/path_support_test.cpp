/**
 *    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::ErrorCodes;
    using mongo::FieldRef;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;
    using mongo::PathSupport;
    using mongo::Status;
    using mongo::StringData;

    class EmptyDoc : public mongo::unittest::Test {
    public:
        EmptyDoc() : _doc() {}

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };


    TEST_F(EmptyDoc, EmptyPath) {
        setField("");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    TEST_F(EmptyDoc, NewField) {
        setField("a");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    class SimpleDoc : public mongo::unittest::Test {
    public:
        SimpleDoc() : _doc() {}

        virtual void setUp() {
            // {a: 1}
            ASSERT_OK(root().appendInt("a", 1));
        }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(SimpleDoc, EmptyPath) {
        setField("");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    TEST_F(SimpleDoc, SimplePath) {
        setField("a");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(SimpleDoc, LongerPath) {
        setField("a.b");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(SimpleDoc, NotCommonPrefix) {
        setField("b");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    class NestedDoc : public mongo::unittest::Test {
    public:
        NestedDoc() : _doc() {}

        virtual void setUp() {
            // {a: {b: {c: 1}}}
            Element elemA = _doc.makeElementObject("a");
            ASSERT_TRUE(elemA.ok());
            Element elemB = _doc.makeElementObject("b");
            ASSERT_TRUE(elemB.ok());
            Element elemC = _doc.makeElementInt("c", 1);
            ASSERT_TRUE(elemC.ok());

            ASSERT_OK(elemB.pushBack(elemC));
            ASSERT_OK(elemA.pushBack(elemB));
            ASSERT_OK(root().pushBack(elemA));
        }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(NestedDoc, SimplePath) {
        setField("a");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(NestedDoc, ShorterPath) {
        setField("a.b");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_EQUALS(idxFound, 1);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]), 0);
    }

    TEST_F(NestedDoc, ExactPath) {
        setField("a.b.c");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]["c"]), 0);
    }

    TEST_F(NestedDoc, LongerPath) {
        //  This would for 'c' to change from NumberInt to Object, which is invalid.
        setField("a.b.c.d");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status.code(), ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]["c"]), 0);

    }

    TEST_F(NestedDoc, NotStartingFromRoot) {
        setField("b.c");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root()["a"], &idxFound, &elemFound));
        ASSERT_EQUALS(idxFound, 1);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]["c"]), 0);
    }

    class ArrayDoc : public mongo::unittest::Test {
    public:
        ArrayDoc() : _doc() {}

        virtual void setUp() {
            // {a: []}
            Element elemA = _doc.makeElementArray("a");
            ASSERT_TRUE(elemA.ok());
            ASSERT_OK(root().pushBack(elemA));

            // {a: [], b: [{c: 1}]}
            Element elemB = _doc.makeElementArray("b");
            ASSERT_TRUE(elemB.ok());
            Element elemObj = _doc.makeElementObject("dummy" /* field name not used in array */);
            ASSERT_TRUE(elemObj.ok());
            ASSERT_OK(elemObj.appendInt("c",1));
            ASSERT_OK(elemB.pushBack(elemObj));
            ASSERT_OK(root().pushBack(elemB));
        }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(ArrayDoc, PathOnEmptyArray) {
        setField("a.0");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(ArrayDoc, PathOnPopulatedArray) {
        setField("b.0");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 1);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"][0]), 0);
    }

    TEST_F(ArrayDoc, MixedArrayAndObjectPath) {
        setField("b.0.c");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"][0]["c"]), 0);
    }

    TEST_F(ArrayDoc, ArrayPaddingNecessary) {
        setField("b.5");

        int32_t idxFound;
        Element elemFound = root();
        ASSERT_OK(PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);
    }

    TEST_F(ArrayDoc, NonNumericPathInArray) {
        setField("b.z");

        int32_t idxFound;
        Element elemFound = root();
        Status status = PathSupport::findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status.code(), ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);
    }

} // unnamed namespace
