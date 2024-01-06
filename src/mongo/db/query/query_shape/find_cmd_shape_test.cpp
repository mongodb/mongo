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

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::query_shape {

namespace {
/**
 * TODO this was stolen from another test. Time for a library?
 * Simplistic redaction strategy for testing which appends the field name to the prefix "REDACT_".
 */
std::string applyHmacForTest(StringData sd) {
    return "REDACT_" + sd.toString();
}

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

class FindCmdShapeTest : public ServiceContextTest {
public:
    void setUp() final {
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::unique_ptr<FindCmdShape> makeShapeFromSort(StringData sortJson) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setSort(fromjson(sortJson));
        auto&& parsedRequest =
            uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    }

    BSONObj sortShape(StringData sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        return shape->components.sort;
    }

    /**
     * Returns the shape of the input sort, or boost::none if the input shape was a natural sort
     * which got converted into a hint.
     */
    boost::optional<BSONObj> maybeRedactedSortShape(StringData sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = applyHmacForTest;
        auto shapeBson = shape->toBson(_expCtx->opCtx, opts, SerializationContext::stateDefault());
        if (auto sortElem = shapeBson["sort"]; !sortElem.eoo()) {
            return sortElem.Obj().getOwned();
        }
        return boost::none;
    }

    BSONObj redactedSortShape(StringData sortJson) {
        return *maybeRedactedSortShape(sortJson);
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(FindCmdShapeTest, NormalSortPattern) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"a.b.c":1,"foo":-1})",
        sortShape(R"({"a.b.c": 1, "foo": -1})"));
}

TEST_F(FindCmdShapeTest, NaturalSortPattern) {
    // $natural sorts are interpreted as a hint. Hints are not part of the shape (but should show up
    // in the query stats key).
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({})",
        sortShape(R"({$natural: 1})"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({})",
        sortShape(R"({$natural: -1})"));
}

TEST_F(FindCmdShapeTest, NaturalSortPatternWithMeta) {
    ASSERT_THROWS_CODE(
        sortShape(R"({$natural: 1, x: {$meta: "textScore"}})"), DBException, ErrorCodes::BadValue);
}

TEST_F(FindCmdShapeTest, MetaPatternWithoutNatural) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"normal":1,"$computed1":{"$meta":"textScore"}})",
        sortShape(R"({normal: 1, x: {$meta: "textScore"}})"));
}

// Here we have one test to ensure that the redaction policy is accepted and applied in the
// query_shape utility, but there are more extensive redaction tests in sort_pattern_test.cpp
TEST_F(FindCmdShapeTest, RespectsRedactionPolicy) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"REDACT_normal":1,"REDACT_y":1})",
        redactedSortShape(R"({normal: 1, y: 1})"));

    // No need to redact $natural. Again, this will be interpreted as a hint, but this test is
    // interesting to ensure the $-prefix of $natural doesn't confuse us.
    ASSERT(!maybeRedactedSortShape(R"({$natural: 1})"));
}

TEST_F(FindCmdShapeTest, NoOptionalArguments) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0, cmdShape->components.optionalArgumentsEncoding());
}

TEST_F(FindCmdShapeTest, AllOptionalArgumentsSetToTrue) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    // Tailable can not be set together with 'singleBatch' option.
    fcr->setSingleBatch(false);
    fcr->setAllowDiskUse(true);
    fcr->setReturnKey(true);
    fcr->setShowRecordId(true);
    fcr->setTailable(true);
    fcr->setAwaitData(true);
    fcr->setMirrored(true);
    fcr->setOplogReplay(true);
    fcr->setLimit(1);
    fcr->setSkip(1);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0x2FFFF, cmdShape->components.optionalArgumentsEncoding());
}

TEST_F(FindCmdShapeTest, AllOptionalArgumentsSetToFalse) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setSingleBatch(false);
    fcr->setAllowDiskUse(false);
    fcr->setReturnKey(false);
    fcr->setShowRecordId(false);
    fcr->setTailable(false);
    fcr->setAwaitData(false);
    fcr->setMirrored(false);
    fcr->setOplogReplay(false);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0x2AAA8, cmdShape->components.optionalArgumentsEncoding());
}

}  // namespace

}  // namespace mongo::query_shape
