/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/unittest/unittest.h"
#include "serialization_options.h"
namespace mongo {
namespace {

std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

auto getExpCtx() {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

TEST(SerializeSortPatternTest, SerializeAndRedactFieldName) {
    auto expCtx = getExpCtx();
    auto sortPattern = SortPattern(fromjson("{val: 1}"), expCtx);
    SerializationOptions opts = {};
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;

    // Most basic sort pattern, confirm that field name gets redacted.
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Confirm that multiple sort fields get redacted.
    sortPattern = SortPattern(fromjson("{val: 1, test: -1, third: -1}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1,"HASH<test>":-1,"HASH<third>":-1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Test sort pattern that contains an expression.
    sortPattern = SortPattern(fromjson("{val: 1, test: {$meta: \"randVal\"}}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<val>":1,"$computed1":{"$meta":"randVal"}})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Sorting by only an expression results in a made up field name in serialization and therefore
    // doesn't get redacted.
    sortPattern = SortPattern(fromjson("{val: {$meta: \"textScore\"}}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$computed0":{"$meta":"textScore"}})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    sortPattern = SortPattern(fromjson("{'a.b.c': 1}"), expCtx);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<a>.HASH<b>.HASH<c>":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));
}

TEST(SerializeSortPatternTest, SerializeNoRedaction) {
    auto expCtx = getExpCtx();
    auto sortPattern = SortPattern(fromjson("{val: 1}"), expCtx);
    SerializationOptions opts = {};
    opts.redactIdentifiers = false;
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"val":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts));

    // Call serialize() with no options.
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"val":1})",
        sortPattern.serialize(SortPattern::SortKeySerialization::kForPipelineSerialization));
}

}  // namespace
}  // namespace mongo
