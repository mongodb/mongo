/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/value_comparator.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ValueComparatorTest, EqualToEvaluatesCorrectly) {
    Value val1("bar"_sd);
    Value val2("bar"_sd);
    Value val3("baz"_sd);
    ASSERT_TRUE(ValueComparator().evaluate(val1 == val2));
    ASSERT_FALSE(ValueComparator().evaluate(val1 == val3));
}

TEST(ValueComparatorTest, EqualToEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1("abc"_sd);
    Value val2("def"_sd);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
}

TEST(ValueComparatorTest, EqualToFunctorEvaluatesCorrectly) {
    ValueComparator valueComparator;
    auto equalFunc = valueComparator.getEqualTo();
    Value val1("bar"_sd);
    Value val2("bar"_sd);
    Value val3("baz"_sd);
    ASSERT_TRUE(equalFunc(val1, val2));
    ASSERT_FALSE(equalFunc(val1, val3));
}

TEST(ValueComparatorTest, EqualToFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueComparator(&collator);
    auto equalFunc = valueComparator.getEqualTo();
    Value val1("abc"_sd);
    Value val2("def"_sd);
    ASSERT_TRUE(equalFunc(val1, val2));
}

TEST(ValueComparatorTest, NotEqualEvaluatesCorrectly) {
    Value val1("bar"_sd);
    Value val2("bar"_sd);
    Value val3("baz"_sd);
    ASSERT_FALSE(ValueComparator().evaluate(val1 != val2));
    ASSERT_TRUE(ValueComparator().evaluate(val1 != val3));
}

TEST(ValueComparatorTest, NotEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1("abc"_sd);
    Value val2("def"_sd);
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val1 != val2));
}

TEST(ValueComparatorTest, LessThanEvaluatesCorrectly) {
    Value val1("a"_sd);
    Value val2("b"_sd);
    ASSERT_TRUE(ValueComparator().evaluate(val1 < val2));
    ASSERT_FALSE(ValueComparator().evaluate(val2 < val1));
}

TEST(ValueComparatorTest, LessThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("za"_sd);
    Value val2("yb"_sd);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 < val2));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val2 < val1));
}

TEST(ValueComparatorTest, LessThanFunctorEvaluatesCorrectly) {
    ValueComparator valueComparator;
    auto lessThanFunc = valueComparator.getLessThan();
    Value val1("a"_sd);
    Value val2("b"_sd);
    ASSERT_TRUE(lessThanFunc(val1, val2));
    ASSERT_FALSE(lessThanFunc(val2, val1));
}

TEST(ValueComparatorTest, LessThanFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ValueComparator valueComparator(&collator);
    auto lessThanFunc = valueComparator.getLessThan();
    Value val1("za"_sd);
    Value val2("yb"_sd);
    ASSERT_TRUE(lessThanFunc(val1, val2));
    ASSERT_FALSE(lessThanFunc(val2, val1));
}

TEST(ValueComparatorTest, LessThanOrEqualEvaluatesCorrectly) {
    Value val1("a"_sd);
    Value val2("a"_sd);
    Value val3("b"_sd);
    ASSERT_TRUE(ValueComparator().evaluate(val1 <= val2));
    ASSERT_TRUE(ValueComparator().evaluate(val2 <= val1));
    ASSERT_TRUE(ValueComparator().evaluate(val1 <= val3));
    ASSERT_FALSE(ValueComparator().evaluate(val3 <= val1));
}

TEST(ValueComparatorTest, LessThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("za"_sd);
    Value val2("za"_sd);
    Value val3("yb"_sd);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 <= val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 <= val1));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 <= val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 <= val1));
}

TEST(ValueComparatorTest, GreaterThanEvaluatesCorrectly) {
    Value val1("b"_sd);
    Value val2("a"_sd);
    ASSERT_TRUE(ValueComparator().evaluate(val1 > val2));
    ASSERT_FALSE(ValueComparator().evaluate(val2 > val1));
}

TEST(ValueComparatorTest, GreaterThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("yb"_sd);
    Value val2("za"_sd);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 > val2));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val2 > val1));
}

TEST(ValueComparatorTest, GreaterThanOrEqualEvaluatesCorrectly) {
    Value val1("b"_sd);
    Value val2("b"_sd);
    Value val3("a"_sd);
    ASSERT_TRUE(ValueComparator().evaluate(val1 >= val2));
    ASSERT_TRUE(ValueComparator().evaluate(val2 >= val1));
    ASSERT_TRUE(ValueComparator().evaluate(val1 >= val3));
    ASSERT_FALSE(ValueComparator().evaluate(val3 >= val1));
}

TEST(ValueComparatorTest, GreaterThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value val1("yb"_sd);
    Value val2("yb"_sd);
    Value val3("za"_sd);
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 >= val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 >= val1));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 >= val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 >= val1));
}

TEST(ValueComparatorTest, OrderedValueSetRespectsTheComparator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ValueComparator valueComparator(&collator);
    ValueSet set = valueComparator.makeOrderedValueSet();
    set.insert(Value("yb"_sd));
    set.insert(Value("za"_sd));

    auto it = set.begin();
    ASSERT_VALUE_EQ(*it, Value("za"_sd));
    ++it;
    ASSERT_VALUE_EQ(*it, Value("yb"_sd));
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
    Value val1(Document{{"foo", "abc"_sd}});
    Value val2(Document{{"foo", "def"_sd}});
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 == val1));
}

TEST(ValueComparatorTest, NestedArrayEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Value val1(std::vector<Value>{Value("a"_sd), Value("b"_sd)});
    Value val2(std::vector<Value>{Value("c"_sd), Value("d"_sd)});
    Value val3(std::vector<Value>{Value("c"_sd), Value("d"_sd), Value("e"_sd)});
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val1 == val2));
    ASSERT_TRUE(ValueComparator(&collator).evaluate(val2 == val1));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val1 == val3));
    ASSERT_FALSE(ValueComparator(&collator).evaluate(val3 == val1));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollator) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    ASSERT_EQ(valueCmp.hash(Value("foo"_sd)), valueCmp.hash(Value("FOO"_sd)));
    ASSERT_NE(valueCmp.hash(Value("foo"_sd)), valueCmp.hash(Value("FOOz"_sd)));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollatorWithNestedObjects) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueCmp(&collator);
    Value val1(Document{{"foo", "abc"_sd}});
    Value val2(Document{{"foo", "def"_sd}});
    ASSERT_EQ(valueCmp.hash(val1), valueCmp.hash(val2));
}

TEST(ValueComparatorTest, ValueHasherRespectsCollatorWithNestedArrays) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    ValueComparator valueCmp(&collator);
    Value val1(std::vector<Value>{Value("a"_sd), Value("b"_sd)});
    Value val2(std::vector<Value>{Value("c"_sd), Value("d"_sd)});
    Value val3(std::vector<Value>{Value("c"_sd), Value("d"_sd), Value("e"_sd)});
    ASSERT_EQ(valueCmp.hash(val1), valueCmp.hash(val2));
    ASSERT_NE(valueCmp.hash(val1), valueCmp.hash(val3));
    ASSERT_NE(valueCmp.hash(val2), valueCmp.hash(val3));
}

TEST(ValueComparatorTest, UnorderedSetOfValueRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    auto set = valueCmp.makeUnorderedValueSet();
    ASSERT_TRUE(set.insert(Value("foo"_sd)).second);
    ASSERT_FALSE(set.insert(Value("FOO"_sd)).second);
    ASSERT_TRUE(set.insert(Value("FOOz"_sd)).second);
    ASSERT_EQ(set.size(), 2U);
    ASSERT_EQ(set.count(Value("FoO"_sd)), 1U);
    ASSERT_EQ(set.count(Value("fooZ"_sd)), 1U);
}

TEST(ValueComparatorTest, UnorderedMapOfValueRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator valueCmp(&toLowerCollator);
    auto map = valueCmp.makeUnorderedValueMap<int>();
    map[Value("foo"_sd)] = 1;
    map[Value("FOO"_sd)] = 2;
    map[Value("FOOz"_sd)] = 3;
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map[Value("FoO"_sd)], 2);
    ASSERT_EQ(map[Value("fooZ"_sd)], 3);
}

TEST(ValueComparatorTest, ComparingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCodeWScope("js code",
                                    BSON("foo"
                                         << "bar"))};
    const Value val2{BSONCodeWScope("js code",
                                    BSON("foo"
                                         << "not bar"))};
    ASSERT_TRUE(comparator.evaluate(val1 != val2));
}

TEST(ValueComparatorTest, HashingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const ValueComparator comparator(&collator);
    const Value val1{BSONCodeWScope("js code",
                                    BSON("foo"
                                         << "bar"))};
    const Value val2{BSONCodeWScope("js code",
                                    BSON("foo"
                                         << "not bar"))};
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

}  // namespace
}  // namespace mongo
