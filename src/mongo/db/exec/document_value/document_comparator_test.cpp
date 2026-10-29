// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document_comparator.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <utility>
#include <vector>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"sv}};
    const Document doc2{{"foo", "bar"sv}};
    const Document doc3{{"foo", "baz"sv}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 == doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 == doc3));
}

TEST(DocumentComparatorTest, EqualToEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"sv}};
    const Document doc2{{"foo", "def"sv}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
}

TEST(DocumentComparatorTest, EqualToFunctorEvaluatesCorrectly) {
    DocumentComparator documentComparator;
    auto equalFunc = documentComparator.getEqualTo();
    Document doc1{{"foo", "bar"sv}};
    Document doc2{{"foo", "bar"sv}};
    Document doc3{{"foo", "baz"sv}};
    ASSERT_TRUE(equalFunc(doc1, doc2));
    ASSERT_FALSE(equalFunc(doc1, doc3));
}

TEST(DocumentComparatorTest, EqualToFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentComparator(&collator);
    auto equalFunc = documentComparator.getEqualTo();
    Document doc1{{"foo", "abc"sv}};
    Document doc2{{"foo", "def"sv}};
    ASSERT_TRUE(equalFunc(doc1, doc2));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "bar"sv}};
    const Document doc2{{"foo", "bar"sv}};
    const Document doc3{{"foo", "baz"sv}};
    ASSERT_FALSE(DocumentComparator().evaluate(doc1 != doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 != doc3));
}

TEST(DocumentComparatorTest, NotEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", "abc"sv}};
    const Document doc2{{"foo", "def"sv}};
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"sv}};
    const Document doc2{{"foo", "b"sv}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"sv}};
    const Document doc2{{"foo", "yb"sv}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 < doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 < doc1));
}

TEST(DocumentComparatorTest, LessThanFunctorEvaluatesCorrectly) {
    DocumentComparator documentComparator;
    auto lessThanFunc = documentComparator.getLessThan();
    Document doc1{{"foo", "a"sv}};
    Document doc2{{"foo", "b"sv}};
    ASSERT_TRUE(lessThanFunc(doc1, doc2));
    ASSERT_FALSE(lessThanFunc(doc2, doc1));
}

TEST(DocumentComparatorTest, LessThanFunctorEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    DocumentComparator documentComparator(&collator);
    auto lessThanFunc = documentComparator.getLessThan();
    Document doc1{{"foo", "za"sv}};
    Document doc2{{"foo", "yb"sv}};
    ASSERT_TRUE(lessThanFunc(doc1, doc2));
    ASSERT_FALSE(lessThanFunc(doc2, doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "a"sv}};
    const Document doc2{{"foo", "a"sv}};
    const Document doc3{{"foo", "b"sv}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, LessThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "za"sv}};
    const Document doc2{{"foo", "za"sv}};
    const Document doc3{{"foo", "yb"sv}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 <= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 <= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 <= doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"sv}};
    const Document doc2{{"foo", "a"sv}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator().evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"sv}};
    const Document doc2{{"foo", "za"sv}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 > doc2));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc2 > doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectly) {
    const Document doc1{{"foo", "b"sv}};
    const Document doc2{{"foo", "b"sv}};
    const Document doc3{{"foo", "a"sv}};
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator().evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator().evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator().evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, GreaterThanOrEqualEvaluatesCorrectlyWithNonSimpleCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    const Document doc1{{"foo", "yb"sv}};
    const Document doc2{{"foo", "yb"sv}};
    const Document doc3{{"foo", "za"sv}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 >= doc1));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 >= doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 >= doc1));
}

TEST(DocumentComparatorTest, OrderedDocumentSetRespectsTheComparator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    DocumentComparator documentComparator(&collator);
    DocumentSet set = documentComparator.makeOrderedDocumentSet();
    set.insert(Document{{"foo", "yb"sv}});
    set.insert(Document{{"foo", "za"sv}});

    auto it = set.begin();
    ASSERT_DOCUMENT_EQ(*it, (Document{{"foo", "za"sv}}));
    ++it;
    ASSERT_DOCUMENT_EQ(*it, (Document{{"foo", "yb"sv}}));
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
    const Document doc1{{"foo", Document{{"foo", "abc"sv}}}};
    const Document doc2{{"foo", Document{{"foo", "def"sv}}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
}

TEST(DocumentComparatorTest, NestedArrayEqualityRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"foo", {"a"sv, "b"sv}}};
    const Document doc2{{"foo", {"c"sv, "d"sv}}};
    const Document doc3{{"foo", {"c"sv, "d"sv, "e"sv}}};
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc1 == doc2));
    ASSERT_TRUE(DocumentComparator(&collator).evaluate(doc2 == doc1));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc1 == doc3));
    ASSERT_FALSE(DocumentComparator(&collator).evaluate(doc3 == doc1));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollator) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    ASSERT_EQ(documentCmp.hash(Document{{"foo", "foo"sv}}),
              documentCmp.hash(Document{{"foo", "FOO"sv}}));
    ASSERT_NE(documentCmp.hash(Document{{"foo", "foo"sv}}),
              documentCmp.hash(Document{{"foo", "FOOz"sv}}));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollatorWithNestedObjects) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentCmp(&collator);
    Document doc1(Document{{"foo", "abc"sv}});
    Document doc2(Document{{"foo", "def"sv}});
    ASSERT_EQ(documentCmp.hash(doc1), documentCmp.hash(doc2));
}

TEST(DocumentComparatorTest, DocumentHasherRespectsCollatorWithNestedArrays) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    DocumentComparator documentCmp(&collator);
    Document doc1{
        {"foo", std::vector<Document>{Document{{"foo", "a"sv}}, Document{{"foo", "b"sv}}}}};
    Document doc2{
        {"foo", std::vector<Document>{Document{{"foo", "c"sv}}, Document{{"foo", "d"sv}}}}};
    Document doc3{{"foo",
                   std::vector<Document>{Document{{"foo", "c"sv}},
                                         Document{{"foo", "d"sv}},
                                         Document{{"foo", "e"sv}}}}};
    ASSERT_EQ(documentCmp.hash(doc1), documentCmp.hash(doc2));
    ASSERT_NE(documentCmp.hash(doc1), documentCmp.hash(doc3));
    ASSERT_NE(documentCmp.hash(doc2), documentCmp.hash(doc3));
}

TEST(DocumentComparatorTest, UnorderedSetOfDocumentRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    auto set = documentCmp.makeUnorderedDocumentSet();
    ASSERT_TRUE(set.insert(Document{{"foo", "foo"sv}}).second);
    ASSERT_FALSE(set.insert(Document{{"foo", "FOO"sv}}).second);
    ASSERT_TRUE(set.insert(Document{{"foo", "FOOz"sv}}).second);
    ASSERT_EQ(set.size(), 2U);
    ASSERT_EQ(set.count(Document{{"foo", "FoO"sv}}), 1U);
    ASSERT_EQ(set.count(Document{{"foo", "fooZ"sv}}), 1U);
}

TEST(DocumentComparatorTest, UnorderedMapOfDocumentRespectsCollation) {
    CollatorInterfaceMock toLowerCollator(CollatorInterfaceMock::MockType::kToLowerString);
    DocumentComparator documentCmp(&toLowerCollator);
    auto map = documentCmp.makeUnorderedDocumentMap<int>();
    map[(Document{{"foo", "foo"sv}})] = 1;
    map[(Document{{"foo", "FOO"sv}})] = 2;
    map[(Document{{"foo", "FOOz"sv}})] = 3;
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map[(Document{{"foo", "FoO"sv}})], 2);
    ASSERT_EQ(map[(Document{{"foo", "fooZ"sv}})], 3);
}

TEST(DocumentComparatorTest, ComparingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const DocumentComparator comparator(&collator);
    const Document doc1{{"a", BSONCodeWScope("js code", BSON("foo" << "bar"))}};
    const Document doc2{{"a", BSONCodeWScope("js code", BSON("foo" << "not bar"))}};
    ASSERT_TRUE(comparator.evaluate(doc1 != doc2));
}

TEST(DocumentComparatorTest, HashingCodeWScopeShouldNotRespectCollation) {
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    const Document doc1{{"a", BSONCodeWScope("js code", BSON("foo" << "bar"))}};
    const Document doc2{{"a", BSONCodeWScope("js code", BSON("foo" << "not bar"))}};
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
