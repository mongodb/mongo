// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/pattern_cmp.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using mongo::mutablebson::Element;
using mongo::mutablebson::sortChildren;

class PatternElemCmpTest : public mongo::unittest::Test {
public:
    PatternElemCmpTest() : _doc(), _size(0) {}

    void setUp() override {
        Element arr = _doc.makeElementArray("x");
        ASSERT_TRUE(arr.ok());
        ASSERT_OK(_doc.root().pushBack(arr));
    }

    void addObj(BSONObj obj) {
        ASSERT_LESS_THAN_OR_EQUALS(_size, 3u);
        _objs[_size] = obj;
        _size++;

        ASSERT_OK(_doc.root()["x"].appendObject(std::string_view(), obj));
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
        {Value("a"sv), Value("b"sv)}, {Value("a"sv), Value("b"sv)}, fromjson("{'': 1}"));
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

    assertExpectedSortResults({Value("abc"sv), Value("acb"sv), Value("cba"sv)},
                              {Value("abc"sv), Value("acb"sv), Value("cba"sv)},
                              sortPattern);
    assertExpectedSortResults({Value("abc"sv), Value("acb"sv), Value("cba"sv)},
                              {Value("cba"sv), Value("acb"sv), Value("abc"sv)},
                              sortPattern,
                              &collator);
}
}  // namespace
}  // namespace mongo
