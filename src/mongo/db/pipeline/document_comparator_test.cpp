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

#include "mongo/db/pipeline/document_comparator.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"}};
    const Document doc2{{"foo", "bar"}};
    const Document doc3{{"foo", "baz"}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 == doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 == doc3));
}

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"}};
    const Document doc2{{"foo", "def"}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"}};
    const Document doc2{{"foo", "bar"}};
    const Document doc3{{"foo", "baz"}};
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 != doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 != doc3));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"}};
    const Document doc2{{"foo", "def"}};
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"}};
    const Document doc2{{"foo", "b"}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"}};
    const Document doc2{{"foo", "yb"}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"}};
    const Document doc2{{"foo", "a"}};
    const Document doc3{{"foo", "b"}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"}};
    const Document doc2{{"foo", "za"}};
    const Document doc3{{"foo", "yb"}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"}};
    const Document doc2{{"foo", "a"}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"}};
    const Document doc2{{"foo", "za"}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"}};
    const Document doc2{{"foo", "b"}};
    const Document doc3{{"foo", "a"}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"}};
    const Document doc2{{"foo", "yb"}};
    const Document doc3{{"foo", "za"}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectlyWithNumbers) {
    const Document doc1{{"foo", 88}};
    const Document doc2{{"foo", 88}};
    const Document doc3{{"foo", 99}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 == doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 == doc3));
}

TEST(DocumentComparatorTest, NestedObjectEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", Document{{"foo", "abc"}}}};
    const Document doc2{{"foo", Document{{"foo", "def"}}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
}

TEST(DocumentComparatorTest, NestedArrayEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", std::vector<Value>{Value("a"), Value("b")}}};
    const Document doc2{{"foo", std::vector<Value>{Value("c"), Value("d")}}};
    const Document doc3{{"foo", std::vector<Value>{Value("c"), Value("d"), Value("e")}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 == doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 == doc1));
}

}  // namespace
}  // namespace mongo
