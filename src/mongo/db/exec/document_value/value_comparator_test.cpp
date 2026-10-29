// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value_comparator.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <utility>
#include <vector>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(ValueComparatorTest, EqualToEvaluatesCorrectly) {
    Value val1("bar"sv);
    Value val2("bar"sv);
    Value val3("baz"sv);
    ASSERT_TRUE(ValueComparator().evaluate(val1 == val2));
    ASSERT_FALSE(ValueComparator().evaluate(val1 == val3));
}

TEST(ValueComparatorTest, EqualToEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1("abc"sv);
    Value val2("def"sv);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
}

TEST(ValueComparatorTest, EqualToFunctorEvaluatesCorrectly) {
    ValueComparator valueComparator;
    auto equalFunc = valueComparator.getEqualTo();
    Value val1("bar"sv);
    Value val2("bar"sv);
    Value val3("baz"sv);
    ASSERT_TRUE(equalFunc(val1, val2));
    ASSERT_FALSE(equalFunc(val1, val3));
}

TEST(ValueComparatorTest, EqualToFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueComparator(&collator);
    auto equalFunc = valueComparator.getEqualTo();
    Value val1("abc"sv);
    Value val2("def"sv);
    ASSERT_TRUE(equalFunc(val1, val2));
}

TEST(ValueComparatorTest, NotEqualEvaluatesCorrectly) {
    Value val1("bar"sv);
    Value val2("bar"sv);
    Value val3("baz"sv);
    ASSERT_FALSE(ValueComparator().evaluate(val1 != val2));
    ASSERT_TRUE(ValueComparator().evaluate(val1 != val3));
}

TEST(ValueComparatorTest, NotEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1("abc"sv);
    Value val2("def"sv);
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val1 != val2));
}

TEST(ValueComparatorTest, LessThanEvaluatesCorrectly) {
    Value val1("a"sv);
    Value val2("b"sv);
    ASSERT_TRUE(ValueComparator().evaluate(val1 < val2));
    ASSERT_FALSE(ValueComparator().evaluate(val2 < val1));
}

TEST(ValueComparatorTest, LessThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("za"sv);
    Value val2("yb"sv);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 < val2));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val2 < val1));
}

TEST(ValueComparatorTest, LessThanFunctorEvaluatesCorrectly) {
    ValueComparator valueComparator;
    auto lessThanFunc = valueComparator.getLessThan();
    Value val1("a"sv);
    Value val2("b"sv);
    ASSERT_TRUE(lessThanFunc(val1, val2));
    ASSERT_FALSE(lessThanFunc(val2, val1));
}

TEST(ValueComparatorTest, LessThanFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ValueComparator valueComparator(&collator);
    auto lessThanFunc = valueComparator.getLessThan();
    Value val1("za"sv);
    Value val2("yb"sv);
    ASSERT_TRUE(lessThanFunc(val1, val2));
    ASSERT_FALSE(lessThanFunc(val2, val1));
}

TEST(ValueComparatorTest, LessThanOrEqualEvaluatesCorrectly) {
    Value val1("a"sv);
    Value val2("a"sv);
    Value val3("b"sv);
    ASSERT_TRUE(ValueComparator().evaluate(val1 <= val2));
    ASSERT_TRUE(ValueComparator().evaluate(val2 <= val1));
    ASSERT_TRUE(ValueComparator().evaluate(val1 <= val3));
    ASSERT_FALSE(ValueComparator().evaluate(val3 <= val1));
}

TEST(ValueComparatorTest, LessThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("za"sv);
    Value val2("za"sv);
    Value val3("yb"sv);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 <= val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 <= val1));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 <= val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 <= val1));
}

TEST(ValueComparatorTest, GreaterThanEvaluatesCorrectly) {
    Value val1("b"sv);
    Value val2("a"sv);
    ASSERT_TRUE(ValueComparator().evaluate(val1 > val2));
    ASSERT_FALSE(ValueComparator().evaluate(val2 > val1));
}

TEST(ValueComparatorTest, GreaterThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("yb"sv);
    Value val2("za"sv);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 > val2));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val2 > val1));
}

TEST(ValueComparatorTest, GreaterThanOrEqualEvaluatesCorrectly) {
    Value val1("b"sv);
    Value val2("b"sv);
    Value val3("a"sv);
    ASSERT_TRUE(ValueComparator().evaluate(val1 >= val2));
    ASSERT_TRUE(ValueComparator().evaluate(val2 >= val1));
    ASSERT_TRUE(ValueComparator().evaluate(val1 >= val3));
    ASSERT_FALSE(ValueComparator().evaluate(val3 >= val1));
}

TEST(ValueComparatorTest, GreaterThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("yb"sv);
    Value val2("yb"sv);
    Value val3("za"sv);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 >= val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 >= val1));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 >= val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 >= val1));
}

TEST(ValueComparatorTest, OrderedValueSetRespectsTheComparator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ValueComparator valueComparator(&collator);
    ValueSet set = valueComparator.makeOrderedValueSet();
    set.insert(Value("yb"sv));
    set.insert(Value("za"sv));

    auto it = set.begin();
    ASSERT_VALUE_EQ(*it, Value("za"sv));
    ++it;
    ASSERT_VALUE_EQ(*it, Value("yb"sv));
    ++it;
    ASSERT(it == set.end());
}

TEST(ValueComparatorTest, EqualToEvaluatesCorrectlyWithNumbers) {
    Value val1(88);
    Value val2(88);
    Value val3(99);
    ASSERT_TRUE(ValueComparator().evaluate(val1 == val2));
    ASSERT_FALSE(ValueComparator().evaluate(val1 == val3));
}

TEST(ValueComparatorTest, NestedObjectEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1(Document{{"foo", "abc"sv}});
    Value val2(Document{{"foo", "def"sv}});
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 == val1));
}

TEST(ValueComparatorTest, NestedArrayEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1(std::vector<Value>{Value("a"sv), Value("b"sv)});
    Value val2(std::vector<Value>{Value("c"sv), Value("d"sv)});
    Value val3(std::vector<Value>{Value("c"sv), Value("d"sv), Value("e"sv)});
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 == val1));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val1 == val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 == val1));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollator) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    ASSERT_EQ(valueCmp.hash(Value("foo"sv)), valueCmp.hash(Value("FOO"sv)));
    ASSERT_NE(valueCmp.hash(Value("foo"sv)), valueCmp.hash(Value("FOOz"sv)));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollatorWithNestedObjects) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueCmp(&collator);
    Value val1(Document{{"foo", "abc"sv}});
    Value val2(Document{{"foo", "def"sv}});
    ASSERT_EQ(valueCmp.hash(val1), valueCmp.hash(val2));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollatorWithNestedArrays) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueCmp(&collator);
    Value val1(std::vector<Value>{Value("a"sv), Value("b"sv)});
    Value val2(std::vector<Value>{Value("c"sv), Value("d"sv)});
    Value val3(std::vector<Value>{Value("c"sv), Value("d"sv), Value("e"sv)});
    ASSERT_EQ(valueCmp.hash(val1), valueCmp.hash(val2));
    ASSERT_NE(valueCmp.hash(val1), valueCmp.hash(val3));
    ASSERT_NE(valueCmp.hash(val2), valueCmp.hash(val3));
}

TEST(ValueComparatorTest, UnorderedSetOfValueRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    auto set = valueCmp.makeFlatUnorderedValueSet();
    ASSERT_TRUE(set.insert(Value("foo"sv)).second);
    ASSERT_FALSE(set.insert(Value("FOO"sv)).second);
    ASSERT_TRUE(set.insert(Value("FOOz"sv)).second);
    ASSERT_EQ(set.size(), 2U);
    ASSERT_EQ(set.count(Value("FoO"sv)), 1U);
    ASSERT_EQ(set.count(Value("fooZ"sv)), 1U);
}

TEST(ValueComparatorTest, UnorderedMapOfValueRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    auto map = valueCmp.makeUnorderedValueMap<int>();
    map[Value("foo"sv)] = 1;
    map[Value("FOO"sv)] = 2;
    map[Value("FOOz"sv)] = 3;
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map[Value("FoO"sv)], 2);
    ASSERT_EQ(map[Value("fooZ"sv)], 3);
}

TEST(ValueComparatorTest, ComparingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCodeWScope("js code", BSON("foo" << "bar"))};
    const Value val2{BSONCodeWScope("js code", BSON("foo" << "not bar"))};
    ASSERT_TRUE(comparator.evaluate(val1 != val2));
}

TEST(ValueComparatorTest, HashingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCodeWScope("js code", BSON("foo" << "bar"))};
    const Value val2{BSONCodeWScope("js code", BSON("foo" << "not bar"))};
    ASSERT_NE(comparator.hash(val1), comparator.hash(val2));
}

TEST(ValueComparatorTest, ComparingCodeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCode("js code")};
    const Value val2{BSONCode("other js code")};
    ASSERT_TRUE(comparator.evaluate(val1 != val2));
}

TEST(ValueComparatorTest, HashingCodeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCode("js code")};
    const Value val2{BSONCode("other js code")};
    ASSERT_NE(comparator.hash(val1), comparator.hash(val2));
}

// This test was originally designed to reproduce SERVER-78126.
TEST(ValueComparatorTest, ArraysDifferingByOneStringShouldHaveDifferentHashes) {
    const ValueComparator comparator{};
    const Value val1{std::vector<Value>{Value{std::string{"a"}}, Value{std::string{"x"}}}};
    const Value val2{std::vector<Value>{Value{std::string{"b"}}, Value{std::string{"x"}}}};
    ASSERT_NE(comparator.compare(val1, val2), 0);
    ASSERT_NE(comparator.hash(val1), comparator.hash(val2));
}

TEST(ValueComparatorTest, ArraysDifferingByOneStringShouldHaveDifferentHashesWithCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const ValueComparator comparator{&collator};
    const Value val1{std::vector<Value>{Value{std::string{"abc"}}, Value{std::string{"xyz"}}}};
    const Value val2{std::vector<Value>{Value{std::string{"bcd"}}, Value{std::string{"xyz"}}}};
    ASSERT_NE(comparator.compare(val1, val2), 0);
    ASSERT_NE(comparator.hash(val1), comparator.hash(val2));
}

TEST(ValueComparatorTest, ObjectsDifferingByOneStringShouldHaveDifferentHashes) {
    const ValueComparator comparator{};
    const Value val1(
        Document({{"foo"sv, Value{std::string{"abc"}}}, {"bar"sv, Value{std::string{"xyz"}}}}));
    const Value val2(
        Document({{"foo"sv, Value{std::string{"def"}}}, {"bar"sv, Value{std::string{"xyz"}}}}));
    ASSERT_NE(comparator.compare(val1, val2), 0);
    ASSERT_NE(comparator.hash(val1), comparator.hash(val2));
}

}  // namespace
}  // namespace mongo
