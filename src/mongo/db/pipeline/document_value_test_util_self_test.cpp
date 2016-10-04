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

#include "mongo/db/pipeline/document_value_test_util.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DocumentValueTestUtilSelfTest, DocumentEQ) {
    ASSERT_DOCUMENT_EQ(Document({{"foo", "bar"}}), Document({{"foo", "bar"}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentNE) {
    ASSERT_DOCUMENT_NE(Document({{"foo", "bar"}}), Document({{"foo", "baz"}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLT) {
    ASSERT_DOCUMENT_LT(Document({{"foo", "bar"}}), Document({{"foo", "baz"}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentLTE) {
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"}}), Document({{"foo", "baz"}}));
    ASSERT_DOCUMENT_LTE(Document({{"foo", "bar"}}), Document({{"foo", "bar"}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGT) {
    ASSERT_DOCUMENT_GT(Document({{"foo", "baz"}}), Document({{"foo", "bar"}}));
}

TEST(DocumentValueTestUtilSelfTest, DocumentGTE) {
    ASSERT_DOCUMENT_GTE(Document({{"foo", "baz"}}), Document({{"foo", "bar"}}));
    ASSERT_DOCUMENT_GTE(Document({{"foo", "bar"}}), Document({{"foo", "bar"}}));
}

TEST(DocumentValueTestUtilSelfTest, ValueEQ) {
    ASSERT_VALUE_EQ(Value("bar"), Value("bar"));
}

TEST(DocumentValueTestUtilSelfTest, ValueNE) {
    ASSERT_VALUE_NE(Value("bar"), Value("baz"));
}

TEST(DocumentValueTestUtilSelfTest, ValueLT) {
    ASSERT_VALUE_LT(Value("bar"), Value("baz"));
}

TEST(DocumentValueTestUtilSelfTest, ValueLTE) {
    ASSERT_VALUE_LTE(Value("bar"), Value("baz"));
    ASSERT_VALUE_LTE(Value("bar"), Value("bar"));
}

TEST(DocumentValueTestUtilSelfTest, ValueGT) {
    ASSERT_VALUE_GT(Value("baz"), Value("bar"));
}

TEST(DocumentValueTestUtilSelfTest, ValueGTE) {
    ASSERT_VALUE_GTE(Value("baz"), Value("bar"));
    ASSERT_VALUE_GTE(Value("bar"), Value("bar"));
}

}  // namespace
}  // namespace mongo
