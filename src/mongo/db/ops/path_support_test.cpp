/**
 *    Copyright 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/ops/path_support.h"

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

    using mongo::BSONObj;
    using mongo::ErrorCodes;
    using mongo::FieldRef;
    using mongo::fromjson;
    using mongo::jstNULL;
    using mongo::NumberInt;
    using mongo::Object;
    using mongo::pathsupport::findLongestPrefix;
    using mongo::pathsupport::createPathAt;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::countChildren;
    using mongo::mutablebson::getNthChild;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;
    using mongoutils::str::stream;
    using std::string;

    class EmptyDoc : public mongo::unittest::Test {
    public:
        EmptyDoc() : _doc() {}

        Document& doc() { return _doc; }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }

        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(EmptyDoc, EmptyPath) {
        setField("");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    TEST_F(EmptyDoc, NewField) {
        setField("a");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);

        Element newElem = doc().makeElementInt("a", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_OK(createPathAt(field(), 0, root(), newElem));
        ASSERT_EQUALS(fromjson("{a: 1}"), doc());
    }

    class SimpleDoc : public mongo::unittest::Test {
    public:
        SimpleDoc() : _doc() {}

        virtual void setUp() {
            // {a: 1}
            ASSERT_OK(root().appendInt("a", 1));
        }

        Document& doc() { return _doc; }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(SimpleDoc, EmptyPath) {
        setField("");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);
    }

    TEST_F(SimpleDoc, SimplePath) {
        setField("a");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(SimpleDoc, LongerPath) {
        setField("a.b");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(SimpleDoc, NotCommonPrefix) {
        setField("b");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status, ErrorCodes::NonExistentPath);

        // From this point on, handles the creation of the '.b' part that wasn't found.
        Element newElem = doc().makeElementInt("b", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(root()), 1u);

        ASSERT_OK(createPathAt(field(), 0, root(), newElem));
        ASSERT_EQUALS(newElem.getFieldName(), "b");
        ASSERT_EQUALS(newElem.getType(), NumberInt);
        ASSERT_TRUE(newElem.hasValue());
        ASSERT_EQUALS(newElem.getValueInt(), 1);

        ASSERT_TRUE(newElem.parent().ok() /* root an ok parent */);
        ASSERT_EQUALS(countChildren(root()), 2u);
        ASSERT_EQUALS(root().leftChild().getFieldName(), "a");
        ASSERT_EQUALS(root().leftChild().rightSibling().getFieldName(), "b");
        ASSERT_EQUALS(root().rightChild().getFieldName(), "b");
        ASSERT_EQUALS(root().rightChild().leftSibling().getFieldName(), "a");
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

        Document& doc() { return _doc; }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }
        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(NestedDoc, SimplePath) {
        setField("a");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(NestedDoc, ShorterPath) {
        setField("a.b");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_EQUALS(idxFound, 1U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]), 0);
    }

    TEST_F(NestedDoc, ExactPath) {
        setField("a.b.c");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]["c"]), 0);
    }

    TEST_F(NestedDoc, LongerPath) {
        //  This would for 'c' to change from NumberInt to Object, which is invalid.
        setField("a.b.c.d");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status.code(), ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]["c"]), 0);

    }

    TEST_F(NestedDoc, NewFieldNested) {
        setField("a.b.d");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_EQUALS(idxFound, 1U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]["b"]), 0);

        // From this point on, handles the creation of the '.d' part that wasn't found.
        Element newElem = doc().makeElementInt("d", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // 'c' is a child of 'b'

        ASSERT_OK(createPathAt(field(), idxFound+1, elemFound, newElem));
        ASSERT_EQUALS(fromjson("{a: {b: {c: 1, d: 1}}}"), doc());
    }

    TEST_F(NestedDoc, NotStartingFromRoot) {
        setField("b.c");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root()["a"], &idxFound, &elemFound));
        ASSERT_EQUALS(idxFound, 1U);
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

        Document& doc() { return _doc; }

        Element root() { return _doc.root(); }

        FieldRef& field() { return _field; }

        void setField(StringData str) { _field.parse(str); }

    private:
        Document _doc;
        FieldRef _field;
    };

    TEST_F(ArrayDoc, PathOnEmptyArray) {
        setField("a.0");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["a"]), 0);
    }

    TEST_F(ArrayDoc, PathOnPopulatedArray) {
        setField("b.0");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 1U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"][0]), 0);
    }

    TEST_F(ArrayDoc, MixedArrayAndObjectPath) {
        setField("b.0.c");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 2U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"][0]["c"]), 0);
    }

    TEST_F(ArrayDoc, ExtendingExistingObject) {
        setField("b.0.d");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 1U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"][0]), 0);

        // From this point on, handles the creation of the '.0.d' part that wasn't found.
        Element newElem = doc().makeElementInt("d", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // '{c:1}' is a child of b.0

        ASSERT_OK(createPathAt(field(), idxFound+1, elemFound, newElem));
        ASSERT_EQUALS(fromjson("{a: [], b: [{c:1, d:1}]}"), doc());
    }

    TEST_F(ArrayDoc, NewObjectInsideArray) {
        setField("b.1.c");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);

        // From this point on, handles the creation of the '.1.c' part that wasn't found.
        Element newElem = doc().makeElementInt("c", 2);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // '{c:1}' is a child of 'b'

        ASSERT_OK(createPathAt(field(), idxFound+1, elemFound, newElem));
        ASSERT_EQUALS(fromjson("{a: [], b: [{c:1},{c:2}]}"), doc());
    }

    TEST_F(ArrayDoc, NewNestedObjectInsideArray) {
        setField("b.1.c.d");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);

        // From this point on, handles the creation of the '.1.c.d' part that wasn't found.
        Element newElem = doc().makeElementInt("d", 2);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // '{c:1}' is a child of 'b'

        ASSERT_OK(createPathAt(field(), idxFound+1, elemFound, newElem));
        ASSERT_EQUALS(fromjson("{a: [], b: [{c:1},{c:{d:2}}]}"), doc());
    }

    TEST_F(ArrayDoc, ArrayPaddingNecessary) {
        setField("b.5");

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);

        // From this point on, handles the creation of the '.5' part that wasn't found.
        Element newElem = doc().makeElementInt("", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // '{c:1}' is a child of 'b'

        ASSERT_OK(createPathAt(field(), idxFound+1, elemFound, newElem));
        ASSERT_EQUALS(fromjson("{a: [], b: [{c:1},null,null,null,null,1]}"), doc());
    }

    TEST_F(ArrayDoc, ExcessivePaddingRequested) {
        // Try to create an array item beyond what we're allowed to pad.
        string paddedField = stream() << "b." << mongo::pathsupport::kMaxPaddingAllowed + 1;;
        setField(paddedField);

        size_t idxFound;
        Element elemFound = root();
        ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));

        // From this point on, try to create the padded part that wasn't found.
        Element newElem = doc().makeElementInt("", 1);
        ASSERT_TRUE(newElem.ok());
        ASSERT_EQUALS(countChildren(elemFound), 1u); // '{c:1}' is a child of 'b'

        Status status = createPathAt(field(), idxFound+1, elemFound, newElem);
        ASSERT_EQUALS(status.code(), ErrorCodes::CannotBackfillArray);
    }

    TEST_F(ArrayDoc, NonNumericPathInArray) {
        setField("b.z");

        size_t idxFound;
        Element elemFound = root();
        Status status = findLongestPrefix(field(), root(), &idxFound, &elemFound);
        ASSERT_EQUALS(status.code(), ErrorCodes::PathNotViable);
        ASSERT_TRUE(elemFound.ok());
        ASSERT_EQUALS(idxFound, 0U);
        ASSERT_EQUALS(elemFound.compareWithElement(root()["b"]), 0);
    }

} // unnamed namespace
