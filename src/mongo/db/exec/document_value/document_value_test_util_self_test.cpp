// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

#include <string_view>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(DocumentValueTestUtilSelfTest, DocumentEQ) {
    ASSERT_DOCUMENT_EQ(Document({{"foo", "bar"sv}}), Document({{"foo", "bar"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentNE) {
    ASSERT_DOCUMENT_NE(Document({{"foo", "bar"sv}}), Document({{"foo", "baz"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLT) {
    ASSERT_DOCUMENT_LT(Document({{"foo", "bar"sv}}), Document({{"foo", "baz"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLTE) {
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"sv}}), Document({{"foo", "baz"sv}}));
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"sv}}), Document({{"foo", "bar"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGT) {
    ASSERT_DOCUMENT_GT(Document({{"foo", "baz"sv}}), Document({{"foo", "bar"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGTE) {
    ASSERT_DOCUMENT_GTE(Document({{"foo", "baz"sv}}), Document({{"foo", "bar"sv}}));
    ASSERT_DOCUMENT_GTE(Document({{"foo", "bar"sv}}), Document({{"foo", "bar"sv}}));
}

TEST(DocumentValueTestUtilSelfTest, ValueEQ) {
    ASSERT_VALUE_EQ(Value("bar"sv), Value("bar"sv));
}

TEST(DocumentValueTestUtilSelfTest, ValueNE) {
    ASSERT_VALUE_NE(Value("bar"sv), Value("baz"sv));
}

TEST(DocumentValueTestUtilSelfTest, ValueLT) {
    ASSERT_VALUE_LT(Value("bar"sv), Value("baz"sv));
}

TEST(DocumentValueTestUtilSelfTest, ValueLTE) {
    ASSERT_VALUE_LTE(Value("bar"sv), Value("baz"sv));
    ASSERT_VALUE_LTE(Value("bar"sv), Value("bar"sv));
}

TEST(DocumentValueTestUtilSelfTest, ValueGT) {
    ASSERT_VALUE_GT(Value("baz"sv), Value("bar"sv));
}

TEST(DocumentValueTestUtilSelfTest, ValueGTE) {
    ASSERT_VALUE_GTE(Value("baz"sv), Value("bar"sv));
    ASSERT_VALUE_GTE(Value("bar"sv), Value("bar"sv));
}

TEST(DocumentValueTestUtilSelfTest, MatcherDocumentsEq) {
    const std::vector<Document> docs({
        Document({{"foo"sv, "bar"sv}}),
        Document({{"quux"sv, "grault"sv}}),
    });
    const std::vector<Document> docs_eq({
        Document({{"foo"sv, "bar"sv}}),
        Document({{"quux"sv, "grault"sv}}),
    });
    const std::vector<Document> docs_longer({Document({{"foo"sv, "bar"sv}}),
                                             Document({{"quux"sv, "grault"sv}}),
                                             Document({{"gub"sv, "slub"sv}})});
    const std::vector<Document> docs_swapped({
        Document({{"quux"sv, "grault"sv}}),
        Document({{"foo"sv, "bar"sv}}),
    });

    const auto match_docs = unittest::detail::DocumentsEq(docs);
    ASSERT_TRUE(testing::Matches(match_docs)(docs));
    ASSERT_TRUE(testing::Matches(match_docs)(docs_eq));
    ASSERT_FALSE(testing::Matches(match_docs)(docs_longer));
    ASSERT_FALSE(testing::Matches(match_docs)(docs_swapped));

    const auto match_eq = unittest::detail::DocumentsEq(docs_eq);
    ASSERT_TRUE(testing::Matches(match_eq)(docs));
    ASSERT_TRUE(testing::Matches(match_eq)(docs_eq));
    ASSERT_FALSE(testing::Matches(match_eq)(docs_longer));
    ASSERT_FALSE(testing::Matches(match_eq)(docs_swapped));

    const auto match_longer = unittest::detail::DocumentsEq(docs_longer);
    ASSERT_FALSE(testing::Matches(match_longer)(docs));
    ASSERT_FALSE(testing::Matches(match_longer)(docs_eq));
    ASSERT_TRUE(testing::Matches(match_longer)(docs_longer));
    ASSERT_FALSE(testing::Matches(match_longer)(docs_swapped));

    const auto match_swapped = unittest::detail::DocumentsEq(docs_swapped);
    ASSERT_FALSE(testing::Matches(match_swapped)(docs));
    ASSERT_FALSE(testing::Matches(match_swapped)(docs_eq));
    ASSERT_FALSE(testing::Matches(match_swapped)(docs_longer));
    ASSERT_TRUE(testing::Matches(match_swapped)(docs_swapped));
}

TEST(DocumentValueUtilSelfTest, AssertDocumentsComparison) {
    const std::vector<Document> docs({
        Document({{"foo"sv, "bar"sv}}),
        Document({{"quux"sv, "grault"sv}}),
    });
    const std::vector<Document> docs_eq({
        Document({{"foo"sv, "bar"sv}}),
        Document({{"quux"sv, "grault"sv}}),
    });
    const std::vector<Document> docs_longer({Document({{"foo"sv, "bar"sv}}),
                                             Document({{"quux"sv, "grault"sv}}),
                                             Document({{"gub"sv, "slub"sv}})});
    const std::vector<Document> docs_swapped({
        Document({{"quux"sv, "grault"sv}}),
        Document({{"foo"sv, "bar"sv}}),
    });

    ASSERT_DOCUMENTS_EQ(docs, docs);
    ASSERT_DOCUMENTS_EQ(docs, docs_eq);
    ASSERT_DOCUMENTS_NE(docs, docs_longer);
    ASSERT_DOCUMENTS_NE(docs, docs_swapped);

    ASSERT_DOCUMENTS_EQ(docs_eq, docs);
    ASSERT_DOCUMENTS_EQ(docs_eq, docs_eq);
    ASSERT_DOCUMENTS_NE(docs_eq, docs_longer);
    ASSERT_DOCUMENTS_NE(docs_eq, docs_swapped);

    ASSERT_DOCUMENTS_NE(docs_longer, docs);
    ASSERT_DOCUMENTS_NE(docs_longer, docs_eq);
    ASSERT_DOCUMENTS_EQ(docs_longer, docs_longer);
    ASSERT_DOCUMENTS_NE(docs_longer, docs_swapped);

    ASSERT_DOCUMENTS_NE(docs_swapped, docs);
    ASSERT_DOCUMENTS_NE(docs_swapped, docs_eq);
    ASSERT_DOCUMENTS_NE(docs_swapped, docs_longer);
    ASSERT_DOCUMENTS_EQ(docs_swapped, docs_swapped);
}

}  // namespace
}  // namespace mongo
