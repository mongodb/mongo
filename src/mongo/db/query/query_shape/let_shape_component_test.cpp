/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/let_shape_component.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

TEST(CmdWithLetShapeTest, SizeOfLetShapeComponent) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto let = fromjson(R"({x: 4, y: "str"})");
    auto components = std::make_unique<LetShapeComponent>(let, expCtx);

    const auto minimumSize = sizeof(CmdSpecificShapeComponents) + sizeof(BSONObj) + sizeof(bool) +
        static_cast<size_t>(components->shapifiedLet.objsize());

    ASSERT_GTE(components->size(), minimumSize);
    ASSERT_LTE(components->size(), minimumSize + 8 /*padding*/);
}

TEST(CmdWithLetShapeTest, SizeOfComponentWithAndWithoutLet) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto let = fromjson(R"({x: 4, y: "str"})");
    auto componentsWithLet = std::make_unique<LetShapeComponent>(let, expCtx);
    auto componentsWithNoLet = std::make_unique<LetShapeComponent>(boost::none, expCtx);

    ASSERT_LT(componentsWithNoLet->size(), componentsWithLet->size());
}

}  // namespace
}  // namespace mongo::query_shape
