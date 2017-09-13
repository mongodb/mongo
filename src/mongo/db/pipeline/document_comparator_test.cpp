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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"_sd}};
    const Document doc2{{"foo", "bar"_sd}};
    const Document doc3{{"foo", "baz"_sd}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 == doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 == doc3));
}

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"_sd}};
    const Document doc2{{"foo", "def"_sd}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
}

TEST(DocumentComparatorTest, EqualToFunctorEvaluatesCorrectly) {
    DocumentComparator documentComparator;
    auto equalFunc = documentComparator.getEqualTo();
    Document doc1{{"foo", "bar"_sd}};
    Document doc2{{"foo", "bar"_sd}};
    Document doc3{{"foo", "baz"_sd}};
    ASSERT_TRUE(equalFunc(doc1, doc2));
    ASSERT_FALSE(equalFunc(doc1, doc3));
}

TEST(DocumentComparatorTest, EqualToFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentComparator(&collator);
    auto equalFunc = documentComparator.getEqualTo();
    Document doc1{{"foo", "abc"_sd}};
    Document doc2{{"foo", "def"_sd}};
    ASSERT_TRUE(equalFunc(doc1, doc2));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"_sd}};
    const Document doc2{{"foo", "bar"_sd}};
    const Document doc3{{"foo", "baz"_sd}};
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 != doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 != doc3));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"_sd}};
    const Document doc2{{"foo", "def"_sd}};
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"_sd}};
    const Document doc2{{"foo", "b"_sd}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"_sd}};
    const Document doc2{{"foo", "yb"_sd}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanFunctorEvaluatesCorrectly) {
    DocumentComparator documentComparator;
    auto lessThanFunc = documentComparator.getLessThan();
    Document doc1{{"foo", "a"_sd}};
    Document doc2{{"foo", "b"_sd}};
    ASSERT_TRUE(lessThanFunc(doc1, doc2));
    ASSERT_FALSE(lessThanFunc(doc2, doc1));
}

TEST(DocumentComparatorTest, LessThanFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    DocumentComparator documentComparator(&collator);
    auto lessThanFunc = documentComparator.getLessThan();
    Document doc1{{"foo", "za"_sd}};
    Document doc2{{"foo", "yb"_sd}};
    ASSERT_TRUE(lessThanFunc(doc1, doc2));
    ASSERT_FALSE(lessThanFunc(doc2, doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"_sd}};
    const Document doc2{{"foo", "a"_sd}};
    const Document doc3{{"foo", "b"_sd}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"_sd}};
    const Document doc2{{"foo", "za"_sd}};
    const Document doc3{{"foo", "yb"_sd}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"_sd}};
    const Document doc2{{"foo", "a"_sd}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"_sd}};
    const Document doc2{{"foo", "za"_sd}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"_sd}};
    const Document doc2{{"foo", "b"_sd}};
    const Document doc3{{"foo", "a"_sd}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"_sd}};
    const Document doc2{{"foo", "yb"_sd}};
    const Document doc3{{"foo", "za"_sd}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, OrderedDocumentSetRespectsTheComparator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    DocumentComparator documentComparator(&collator);
    DocumentSet set = documentComparator.makeOrderedDocumentSet();
    set.insert(Document{{"foo", "yb"_sd}});
    set.insert(Document{{"foo", "za"_sd}});

    auto it = set.begin();
    ASSERT_DOCUMENT_EQ(*it, (Document{{"foo", "za"_sd}}));
    ++it;
    ASSERT_DOCUMENT_EQ(*it, (Document{{"foo", "yb"_sd}}));
    ++it;
    ASSERT(it == set.end());
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
    const Document doc1{{"foo", Document{{"foo", "abc"_sd}}}};
    const Document doc2{{"foo", Document{{"foo", "def"_sd}}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
}

TEST(DocumentComparatorTest, NestedArrayEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", std::vector<Value>{Value("a"_sd), Value("b"_sd)}}};
    const Document doc2{{"foo", std::vector<Value>{Value("c"_sd), Value("d"_sd)}}};
    const Document doc3{{"foo", std::vector<Value>{Value("c"_sd), Value("d"_sd), Value("e"_sd)}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 == doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 == doc1));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollator) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    ASSERT_EQ(documentCmp.hash(Document{{"foo", "foo"_sd}}),
              documentCmp.hash(Document{{"foo", "FOO"_sd}}));
    ASSERT_NE(documentCmp.hash(Document{{"foo", "foo"_sd}}),
              documentCmp.hash(Document{{"foo", "FOOz"_sd}}));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollatorWithNestedObjects) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentCmp(&collator);
    Document doc1(Document{{"foo", "abc"_sd}});
    Document doc2(Document{{"foo", "def"_sd}});
    ASSERT_EQ(documentCmp.hash(doc1), documentCmp.hash(doc2));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollatorWithNestedArrays) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentCmp(&collator);
    Document doc1{
        {"foo", std::vector<Document>{Document{{"foo", "a"_sd}}, Document{{"foo", "b"_sd}}}}};
    Document doc2{
        {"foo", std::vector<Document>{Document{{"foo", "c"_sd}}, Document{{"foo", "d"_sd}}}}};
    Document doc3{{"foo",
                   std::vector<Document>{Document{{"foo", "c"_sd}},
                                         Document{{"foo", "d"_sd}},
                                         Document{{"foo", "e"_sd}}}}};
    ASSERT_EQ(documentCmp.hash(doc1), documentCmp.hash(doc2));
    ASSERT_NE(documentCmp.hash(doc1), documentCmp.hash(doc3));
    ASSERT_NE(documentCmp.hash(doc2), documentCmp.hash(doc3));
}

TEST(DocumentComparatorTest, UnorderedSetOfDocumentRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    auto set = documentCmp.makeUnorderedDocumentSet();
    ASSERT_TRUE(set.insert(Document{{"foo", "foo"_sd}}).second);
    ASSERT_FALSE(set.insert(Document{{"foo", "FOO"_sd}}).second);
    ASSERT_TRUE(set.insert(Document{{"foo", "FOOz"_sd}}).second);
    ASSERT_EQ(set.size(), 2U);
    ASSERT_EQ(set.count(Document{{"foo", "FoO"_sd}}), 1U);
    ASSERT_EQ(set.count(Document{{"foo", "fooZ"_sd}}), 1U);
}

TEST(DocumentComparatorTest, UnorderedMapOfDocumentRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    auto map = documentCmp.makeUnorderedDocumentMap<int>();
    map[(Document{{"foo", "foo"_sd}})] = 1;
    map[(Document{{"foo", "FOO"_sd}})] = 2;
    map[(Document{{"foo", "FOOz"_sd}})] = 3;
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map[(Document{{"foo", "FoO"_sd}})], 2);
    ASSERT_EQ(map[(Document{{"foo", "fooZ"_sd}})], 3);
}

TEST(DocumentComparatorTest, ComparingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const DocumentComparator comparator(&collator);
    const Document doc1{{"a",
                         BSONCodeWScope("js code",
                                        BSON("foo"
                                             << "bar"))}};
    const Document doc2{{"a",
                         BSONCodeWScope("js code",
                                        BSON("foo"
                                             << "not bar"))}};
    ASSERT_TRUE(comparator.evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, HashingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"a",
                         BSONCodeWScope("js code",
                                        BSON("foo"
                                             << "bar"))}};
    const Document doc2{{"a",
                         BSONCodeWScope("js code",
                                        BSON("foo"
                                             << "not bar"))}};
    size_t seed1, seed2 = 0;
    doc1.hash_combine(seed1, &collator);
    doc2.hash_combine(seed2, &collator);
    ASSERT_NE(seed1, seed2);
}

TEST(DocumentComparatorTest, ComparingCodeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const DocumentComparator comparator(&collator);
    const Document doc1{{"a", BSONCode("js code")}};
    const Document doc2{{"a", BSONCode("other js code")}};
    ASSERT_TRUE(comparator.evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, HashingCodeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"a", BSONCode("js code")}};
    const Document doc2{{"a", BSONCode("other js code")}};
    size_t seed1, seed2 = 0;
    doc1.hash_combine(seed1, &collator);
    doc2.hash_combine(seed2, &collator);
    ASSERT_NE(seed1, seed2);
}

}  // namespace
}  // namespace mongo
