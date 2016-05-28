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

#include <cstdint>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mutablebson;
using namespace pathsupport;
using mongoutils::str::stream;
using std::unique_ptr;
using std::string;

class EmptyDoc : public mongo::unittest::Test {
public:
    EmptyDoc() : _doc() {}

    Document& doc() {
        return _doc;
    }

    Element root() {
        return _doc.root();
    }

    FieldRef& field() {
        return _field;
    }

    void setField(StringData str) {
        _field.parse(str);
    }

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

    Document& doc() {
        return _doc;
    }

    Element root() {
        return _doc.root();
    }

    FieldRef& field() {
        return _field;
    }
    void setField(StringData str) {
        _field.parse(str);
    }

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

    Document& doc() {
        return _doc;
    }

    Element root() {
        return _doc.root();
    }

    FieldRef& field() {
        return _field;
    }
    void setField(StringData str) {
        _field.parse(str);
    }

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
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // 'c' is a child of 'b'

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));
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
        ASSERT_OK(elemObj.appendInt("c", 1));
        ASSERT_OK(elemB.pushBack(elemObj));
        ASSERT_OK(root().pushBack(elemB));
    }

    Document& doc() {
        return _doc;
    }

    Element root() {
        return _doc.root();
    }

    FieldRef& field() {
        return _field;
    }

    void setField(StringData str) {
        _field.parse(str);
    }

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
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // '{c:1}' is a child of b.0

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));
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
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // '{c:1}' is a child of 'b'

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));
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
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // '{c:1}' is a child of 'b'

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));
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
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // '{c:1}' is a child of 'b'

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));
    ASSERT_EQUALS(fromjson("{a: [], b: [{c:1},null,null,null,null,1]}"), doc());
}

TEST_F(ArrayDoc, ExcessivePaddingRequested) {
    // Try to create an array item beyond what we're allowed to pad. The index is two beyond the max
    // padding since the array already has one element.
    string paddedField = stream() << "b." << mongo::pathsupport::kMaxPaddingAllowed + 2;
    setField(paddedField);

    size_t idxFound;
    Element elemFound = root();
    ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));

    // From this point on, try to create the padded part that wasn't found.
    Element newElem = doc().makeElementInt("", 1);
    ASSERT_TRUE(newElem.ok());
    ASSERT_EQUALS(countChildren(elemFound), 1u);  // '{c:1}' is a child of 'b'

    Status status = createPathAt(field(), idxFound + 1, elemFound, newElem);
    ASSERT_EQUALS(status.code(), ErrorCodes::CannotBackfillArray);
}

TEST_F(ArrayDoc, ExcessivePaddingNotRequestedIfArrayAlreadyPadded) {
    // We will try to set an array element whose index is 5 beyond the max padding.
    string paddedField = stream() << "a." << mongo::pathsupport::kMaxPaddingAllowed + 5;
    setField(paddedField);

    // Add 5 elements to the array.
    for (size_t i = 0; i < 5; ++i) {
        Element arrayA = doc().root().leftChild();
        ASSERT_EQ(arrayA.getFieldName(), "a");
        ASSERT_EQ(arrayA.getType(), mongo::Array);
        arrayA.appendInt("", 1);
    }

    size_t idxFound;
    Element elemFound = root();
    ASSERT_OK(findLongestPrefix(field(), root(), &idxFound, &elemFound));
    ASSERT_TRUE(elemFound.ok());
    ASSERT_EQUALS(countChildren(elemFound), 5u);

    Element newElem = doc().makeElementInt("", 99);
    ASSERT_TRUE(newElem.ok());

    ASSERT_OK(createPathAt(field(), idxFound + 1, elemFound, newElem));

    // Array should now have maxPadding + 6 elements, since the highest array index is maxPadding +
    // 5. maxPadding of these elements are nulls adding as padding, 5 were appended at the
    // beginning, and 1 was added by createPathAt().
    ASSERT_EQ(countChildren(doc().root().leftChild()), mongo::pathsupport::kMaxPaddingAllowed + 6);
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

//
// Tests of equality extraction from MatchExpressions
// NONGOAL: Testing query/match expression parsing and optimization
//

static MatchExpression* makeExpr(const BSONObj& exprBSON) {
    const CollatorInterface* collator = nullptr;
    return MatchExpressionParser::parse(exprBSON, ExtensionsCallbackDisallowExtensions(), collator)
        .getValue()
        .release();
}

static void assertContains(const EqualityMatches& equalities, const BSONObj& wrapped) {
    BSONElement value = wrapped.firstElement();
    StringData path = value.fieldNameStringData();

    EqualityMatches::const_iterator it = equalities.find(path);
    if (it == equalities.end()) {
        FAIL(stream() << "Equality matches did not contain path \"" << path << "\"");
    }
    if (!it->second->getData().valuesEqual(value)) {
        FAIL(stream() << "Equality match at path \"" << path << "\" contains value "
                      << it->second->getData()
                      << ", not value "
                      << value);
    }
}

static void assertContains(const EqualityMatches& equalities, StringData path, int value) {
    assertContains(equalities, BSON(path << value));
}

// NOTE: For tests below, BSONObj expr must exist for lifetime of MatchExpression

TEST(ExtractEqualities, Basic) {
    BSONObj exprBSON = fromjson("{a:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "a", 1);
}

TEST(ExtractEqualities, Multiple) {
    BSONObj exprBSON = fromjson("{a:1, b:2}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 2u);
    assertContains(equalities, "a", 1);
    assertContains(equalities, "b", 2);
}

TEST(ExtractEqualities, EqOperator) {
    BSONObj exprBSON = fromjson("{a:{$eq:1}}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "a", 1);
}

TEST(ExtractEqualities, AndOperator) {
    BSONObj exprBSON = fromjson("{$and:[{a:{$eq:1}},{b:2}]}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 2u);
    assertContains(equalities, "a", 1);
    assertContains(equalities, "b", 2);
}

TEST(ExtractEqualities, NestedAndOperator) {
    BSONObj exprBSON = fromjson("{$and:[{$and:[{a:{$eq:1}},{b:2}]},{c:3}]}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 3u);
    assertContains(equalities, "a", 1);
    assertContains(equalities, "b", 2);
    assertContains(equalities, "c", 3);
}

TEST(ExtractEqualities, NestedPaths) {
    BSONObj exprBSON = fromjson("{'a.a':1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "a.a", 1);
}

TEST(ExtractEqualities, SiblingPaths) {
    BSONObj exprBSON = fromjson("{'a.a':1,'a.b':{$eq:2}}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 2u);
    assertContains(equalities, "a.a", 1);
    assertContains(equalities, "a.b", 2);
}

TEST(ExtractEqualities, NestedAndNestedPaths) {
    BSONObj exprBSON = fromjson("{$and:[{$and:[{'a.a':{$eq:1}},{'a.b':2}]},{'c.c.c':3}]}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 3u);
    assertContains(equalities, "a.a", 1);
    assertContains(equalities, "a.b", 2);
    assertContains(equalities, "c.c.c", 3);
}

TEST(ExtractEqualities, IdOnly) {
    BSONObj exprBSON = fromjson("{_id:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "_id", 1);
}

/**
 * Helper class to allow easy construction of immutable paths
 */
class ImmutablePaths {
public:
    ImmutablePaths() {}

    void addPath(const string& path) {
        _ownedPaths.mutableVector().push_back(new FieldRef(path));
        FieldRef const* conflictPath = NULL;
        ASSERT(_immutablePathSet.insert(_ownedPaths.vector().back(), &conflictPath));
    }

    const FieldRefSet& getPathSet() {
        return _immutablePathSet;
    }

private:
    FieldRefSet _immutablePathSet;
    OwnedPointerVector<FieldRef> _ownedPaths;
};

TEST(ExtractEqualities, IdOnlyMulti) {
    BSONObj exprBSON = fromjson("{_id:{$eq:1},a:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("_id");

    EqualityMatches equalities;
    ASSERT_OK(extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "_id", 1);
}

TEST(ExtractEqualities, IdOnlyIgnoreConflict) {
    BSONObj exprBSON = fromjson("{_id:1,a:1,'a.b':1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("_id");

    EqualityMatches equalities;
    ASSERT_OK(extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "_id", 1);
}

TEST(ExtractEqualities, IdOnlyNested) {
    BSONObj exprBSON = fromjson("{'_id.a':1,'_id.b':{$eq:2},c:3}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("_id");

    EqualityMatches equalities;
    Status status = extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities);
    ASSERT_EQUALS(status.code(), ErrorCodes::NotExactValueField);
}

TEST(ExtractEqualities, IdAndOtherImmutable) {
    BSONObj exprBSON = fromjson("{_id:1,a:1,b:2}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("_id");
    immutablePaths.addPath("a");

    EqualityMatches equalities;
    ASSERT_OK(extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities));
    ASSERT_EQUALS(equalities.size(), 2u);
    assertContains(equalities, "_id", 1);
    assertContains(equalities, "a", 1);
}

TEST(ExtractEqualities, IdAndNestedImmutable) {
    BSONObj exprBSON = fromjson("{_id:1,a:1,'c.d':3}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("_id");
    immutablePaths.addPath("a.b");
    immutablePaths.addPath("c.d");

    EqualityMatches equalities;
    ASSERT_OK(extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities));
    ASSERT_EQUALS(equalities.size(), 3u);
    assertContains(equalities, "_id", 1);
    assertContains(equalities, "a", 1);
    assertContains(equalities, "c.d", 3);
}

TEST(ExtractEqualities, NonFullImmutable) {
    BSONObj exprBSON = fromjson("{'a.b':1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    ImmutablePaths immutablePaths;
    immutablePaths.addPath("a");

    EqualityMatches equalities;
    Status status = extractFullEqualityMatches(*expr, immutablePaths.getPathSet(), &equalities);
    ASSERT_EQUALS(status.code(), ErrorCodes::NotExactValueField);
}

TEST(ExtractEqualities, Empty) {
    BSONObj exprBSON = fromjson("{'':0}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 1u);
    assertContains(equalities, "", 0);
}

TEST(ExtractEqualities, EmptyMulti) {
    BSONObj exprBSON = fromjson("{'':0,a:{$eq:1}}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));
    ASSERT_EQUALS(equalities.size(), 2u);
    assertContains(equalities, "", 0);
    assertContains(equalities, "a", 1);
}

TEST(ExtractEqualities, EqConflict) {
    BSONObj exprBSON = fromjson("{a:1,a:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_EQUALS(extractEqualityMatches(*expr, &equalities).code(),
                  ErrorCodes::NotSingleValueField);
}

TEST(ExtractEqualities, PrefixConflict) {
    BSONObj exprBSON = fromjson("{a:1,'a.b':{$eq:1}}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_EQUALS(extractEqualityMatches(*expr, &equalities).code(),
                  ErrorCodes::NotSingleValueField);
}

TEST(ExtractEqualities, AndPrefixConflict) {
    BSONObj exprBSON = fromjson("{$and:[{a:1},{'a.b':{$eq:1}}]}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_EQUALS(extractEqualityMatches(*expr, &equalities).code(),
                  ErrorCodes::NotSingleValueField);
}

TEST(ExtractEqualities, EmptyConflict) {
    BSONObj exprBSON = fromjson("{'':0,'':{$eq:0}}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));

    EqualityMatches equalities;
    ASSERT_EQUALS(extractEqualityMatches(*expr, &equalities).code(),
                  ErrorCodes::NotSingleValueField);
}

//
// Tests for finding parent equality from equalities found in expression
// NONGOALS: Testing complex equality match extraction - tested above
//

static void assertParent(const EqualityMatches& equalities,
                         StringData pathStr,
                         const BSONObj& wrapped) {
    FieldRef path(pathStr);
    BSONElement value = wrapped.firstElement();
    StringData parentPath = value.fieldNameStringData();

    int parentPathPart;
    BSONElement parentEl = findParentEqualityElement(equalities, path, &parentPathPart);

    if (parentEl.eoo()) {
        FAIL(stream() << "Equality matches did not contain parent for \"" << pathStr << "\"");
    }

    StringData foundParentPath = path.dottedSubstring(0, parentPathPart);
    if (foundParentPath != parentPath) {
        FAIL(stream() << "Equality match parent at path \"" << foundParentPath
                      << "\" does not match \""
                      << parentPath
                      << "\"");
    }

    if (!parentEl.valuesEqual(value)) {
        FAIL(stream() << "Equality match parent for \"" << pathStr << "\" at path \"" << parentPath
                      << "\" contains value "
                      << parentEl
                      << ", not value "
                      << value);
    }
}

static void assertParent(const EqualityMatches& equalities,
                         StringData path,
                         StringData parentPath,
                         int value) {
    assertParent(equalities, path, BSON(parentPath << value));
}

static void assertNoParent(const EqualityMatches& equalities, StringData pathStr) {
    FieldRef path(pathStr);

    int parentPathPart;
    BSONElement parentEl = findParentEqualityElement(equalities, path, &parentPathPart);

    if (!parentEl.eoo()) {
        StringData foundParentPath = path.dottedSubstring(0, parentPathPart);
        FAIL(stream() << "Equality matches contained parent for \"" << pathStr << "\" at \""
                      << foundParentPath
                      << "\"");
    }
}


TEST(FindParentEquality, Basic) {
    BSONObj exprBSON = fromjson("{a:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertNoParent(equalities, "");
    assertParent(equalities, "a", "a", 1);
    assertParent(equalities, "a.b", "a", 1);
}

TEST(FindParentEquality, Multi) {
    BSONObj exprBSON = fromjson("{a:1,b:2}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertNoParent(equalities, "");
    assertParent(equalities, "a", "a", 1);
    assertParent(equalities, "a.b", "a", 1);
    assertParent(equalities, "b", "b", 2);
    assertParent(equalities, "b.b", "b", 2);
}

TEST(FindParentEquality, Nested) {
    BSONObj exprBSON = fromjson("{'a.a':1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertNoParent(equalities, "");
    assertNoParent(equalities, "a");
    assertParent(equalities, "a.a", "a.a", 1);
    assertParent(equalities, "a.a.b", "a.a", 1);
}

TEST(FindParentEquality, NestedMulti) {
    BSONObj exprBSON = fromjson("{'a.a':1,'a.b':2,'c.c':3}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertNoParent(equalities, "");
    assertNoParent(equalities, "a");
    assertNoParent(equalities, "c");
    assertParent(equalities, "a.a", "a.a", 1);
    assertParent(equalities, "a.a.a", "a.a", 1);
    assertParent(equalities, "a.b", "a.b", 2);
    assertParent(equalities, "a.b.b", "a.b", 2);
    assertParent(equalities, "c.c", "c.c", 3);
    assertParent(equalities, "c.c.c", "c.c", 3);
}

TEST(FindParentEquality, Empty) {
    BSONObj exprBSON = fromjson("{'':0}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertParent(equalities, "", "", 0);
}

TEST(FindParentEquality, EmptyMulti) {
    BSONObj exprBSON = fromjson("{'':0,a:1}");
    unique_ptr<MatchExpression> expr(makeExpr(exprBSON));
    EqualityMatches equalities;
    ASSERT_OK(extractEqualityMatches(*expr, &equalities));

    assertParent(equalities, "", "", 0);
    assertParent(equalities, "a", "a", 1);
    assertParent(equalities, "a.b", "a", 1);
}

}  // unnamed namespace
