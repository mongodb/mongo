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

#include "mongo/db/update/pattern_cmp.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using mongo::mutablebson::Element;
using mongo::mutablebson::sortChildren;

class PatternElemCmpTest : public mongo::unittest::Test {
public:
    PatternElemCmpTest() : _doc(), _size(0) {}

    virtual void setUp() {
        Element arr = _doc.makeElementArray("x");
        ASSERT_TRUE(arr.ok());
        ASSERT_OK(_doc.root().pushBack(arr));
    }

    void addObj(BSONObj obj) {
        ASSERT_LESS_THAN_OR_EQUALS(_size, 3u);
        _objs[_size] = obj;
        _size++;

        ASSERT_OK(_doc.root()["x"].appendObject(mongo::StringData(), obj));
    }

    BSONObj getOrigObj(size_t i) {
        return _objs[i];
    }

    BSONObj getSortedObj(size_t i) {
        return getArray()[i].getValueObject();
    }

    Element getArray() {
        return _doc.root()["x"];
    }

private:
    mutablebson::Document _doc;
    BSONObj _objs[3];
    size_t _size;
};

TEST_F(PatternElemCmpTest, PatternElementCmpNormalOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a':1,'b':1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(2));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(PatternElemCmpTest, PatternElementCmpMixedOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{b:1,a:-1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(1));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(PatternElemCmpTest, PatternElementCmpExtraFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, c:2, a:1}"));
    addObj(fromjson("{c:1, a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{a:1,b:1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(2));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(PatternElemCmpTest, PatternElementCmpMissingFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:2, b:2}"));
    addObj(fromjson("{a:1}"));
    addObj(fromjson("{a:3, b:3, c:3}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{b:1,c:1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(PatternElemCmpTest, PatternElementCmpNestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b':1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(PatternElemCmpTest, PatternElementCmpSimpleNestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b: -1}}"));
    addObj(fromjson("{a:{b: -100}}"));
    addObj(fromjson("{a:{b: 34}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b':1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(PatternElemCmpTest, PatternElementCmpNestedInnerObjectDescending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b.d':-1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(2));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(PatternElemCmpTest, PatternElementCmpNestedInnerObjectAscending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b.d':1}"), collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(1));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(2));
}

TEST_F(PatternElemCmpTest, PatternElementCmpSortRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addObj(fromjson("{a: 'abg'}"));
    addObj(fromjson("{a: 'aca'}"));
    addObj(fromjson("{a: 'adc'}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{a: 1}"), &collator));

    ASSERT_BSONOBJ_EQ(getOrigObj(0), getSortedObj(2));
    ASSERT_BSONOBJ_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_BSONOBJ_EQ(getOrigObj(2), getSortedObj(1));
}

static void assertExpectedSortResults(std::vector<Value> originalArr,
                                      std::vector<Value> expectedArr,
                                      BSONObj sortPattern,
                                      const CollatorInterface* collator = nullptr) {
    // For testing sorting we do not need to specify the exact 'originalElement' BSONElement for
    // 'PatternValueCmp' and only care about the sort pattern and the collator (if any).
    const auto sortBy = PatternValueCmp(sortPattern, BSONElement(), collator);

    std::sort(originalArr.begin(), originalArr.end(), sortBy);

    ASSERT_EQUALS(originalArr.size(), expectedArr.size());
    for (size_t idx = 0; idx < originalArr.size(); ++idx) {
        ASSERT_VALUE_EQ(originalArr[idx], expectedArr[idx]);
    }
}

TEST(PatternValueCmpTest, PatternValueCmpAscendingOrder) {
    assertExpectedSortResults(
        {Value(3), Value(2), Value(1)}, {Value(1), Value(2), Value(3)}, fromjson("{'': 1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpDescendingOrder) {
    assertExpectedSortResults(
        {Value(1), Value(2), Value(3)}, {Value(3), Value(2), Value(1)}, fromjson("{'': -1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpStrings) {
    assertExpectedSortResults(
        {Value("a"_sd), Value("b"_sd)}, {Value("a"_sd), Value("b"_sd)}, fromjson("{'': 1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpObjects) {
    assertExpectedSortResults(
        {Value(fromjson("{a: 3}")), Value(fromjson("{a: 2}")), Value(fromjson("{a: 1}"))},
        {Value(fromjson("{a: 1}")), Value(fromjson("{a: 2}")), Value(fromjson("{a: 3}"))},
        fromjson("{a: 1}"));
    assertExpectedSortResults({Value(fromjson("{}")), Value(fromjson("[]")), Value(fromjson("[]"))},
                              {Value(fromjson("[]")), Value(fromjson("[]")), Value(fromjson("{}"))},
                              fromjson("{'': -1}"));
    assertExpectedSortResults({Value(fromjson("{}")), Value(fromjson("{}"))},
                              {Value(fromjson("{}")), Value(fromjson("{}"))},
                              fromjson("{'': -1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpObjectsDottedPath) {
    std::vector<Value> arr = {Value(fromjson("{a: {b: 1}}")),
                              Value(fromjson("{a: {b: 2}}")),
                              Value(fromjson("{a: {b: 3}}"))};

    assertExpectedSortResults(arr,
                              {Value(fromjson("{a: {b: 3}}")),
                               Value(fromjson("{a: {b: 2}}")),
                               Value(fromjson("{a: {b: 1}}"))},
                              fromjson("{'a.b': -1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpObjectsPathDoesNotExist) {
    std::vector<Value> arr = {Value(fromjson("{a: 2}")),
                              Value(fromjson("{b: 2}")),
                              Value(fromjson("{a: 1}")),
                              Value(fromjson("{c: 0}"))};

    assertExpectedSortResults(arr,
                              {Value(fromjson("{b: 2}")),
                               Value(fromjson("{c: 0}")),
                               Value(fromjson("{a: 1}")),
                               Value(fromjson("{a: 2}"))},
                              fromjson("{a: 1}"));

    assertExpectedSortResults(arr,
                              {Value(fromjson("{a: 2}")),
                               Value(fromjson("{a: 1}")),
                               Value(fromjson("{b: 2}")),
                               Value(fromjson("{c: 0}"))},
                              fromjson("{a: -1}"));
}

TEST(PatternValueCmpTest, PatternValueCmpWithCollator) {
    const auto sortPattern = fromjson("{'': 1}");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    assertExpectedSortResults({Value("abc"_sd), Value("acb"_sd), Value("cba"_sd)},
                              {Value("abc"_sd), Value("acb"_sd), Value("cba"_sd)},
                              sortPattern);
    assertExpectedSortResults({Value("abc"_sd), Value("acb"_sd), Value("cba"_sd)},
                              {Value("cba"_sd), Value("acb"_sd), Value("abc"_sd)},
                              sortPattern,
                              &collator);
}
}  // namespace
}  // namespace mongo
