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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

TEST(DocumentValueTestUtilSelfTest, DocumentEQ) {
    ASSERT_DOCUMENT_EQ(Document({{"foo", "bar"_sd}}), Document({{"foo", "bar"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentNE) {
    ASSERT_DOCUMENT_NE(Document({{"foo", "bar"_sd}}), Document({{"foo", "baz"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLT) {
    ASSERT_DOCUMENT_LT(Document({{"foo", "bar"_sd}}), Document({{"foo", "baz"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLTE) {
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"_sd}}), Document({{"foo", "baz"_sd}}));
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"_sd}}), Document({{"foo", "bar"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGT) {
    ASSERT_DOCUMENT_GT(Document({{"foo", "baz"_sd}}), Document({{"foo", "bar"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGTE) {
    ASSERT_DOCUMENT_GTE(Document({{"foo", "baz"_sd}}), Document({{"foo", "bar"_sd}}));
    ASSERT_DOCUMENT_GTE(Document({{"foo", "bar"_sd}}), Document({{"foo", "bar"_sd}}));
}

TEST(DocumentValueTestUtilSelfTest, ValueEQ) {
    ASSERT_VALUE_EQ(Value("bar"_sd), Value("bar"_sd));
}

TEST(DocumentValueTestUtilSelfTest, ValueNE) {
    ASSERT_VALUE_NE(Value("bar"_sd), Value("baz"_sd));
}

TEST(DocumentValueTestUtilSelfTest, ValueLT) {
    ASSERT_VALUE_LT(Value("bar"_sd), Value("baz"_sd));
}

TEST(DocumentValueTestUtilSelfTest, ValueLTE) {
    ASSERT_VALUE_LTE(Value("bar"_sd), Value("baz"_sd));
    ASSERT_VALUE_LTE(Value("bar"_sd), Value("bar"_sd));
}

TEST(DocumentValueTestUtilSelfTest, ValueGT) {
    ASSERT_VALUE_GT(Value("baz"_sd), Value("bar"_sd));
}

TEST(DocumentValueTestUtilSelfTest, ValueGTE) {
    ASSERT_VALUE_GTE(Value("baz"_sd), Value("bar"_sd));
    ASSERT_VALUE_GTE(Value("bar"_sd), Value("bar"_sd));
}

}  // namespace
}  // namespace mongo
