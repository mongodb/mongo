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

}  // namespace
}  // namespace mongo
