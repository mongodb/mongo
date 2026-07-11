// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

TEST(CmdWithLetShapeTest, SizeOfComponentWithSystemVarLet) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto let = fromjson(R"({ROOT: 4})");
    auto componentsWithLet = std::make_unique<LetShapeComponent>(let, expCtx);
    auto componentsWithNoLet = std::make_unique<LetShapeComponent>(boost::none, expCtx);

    ASSERT_EQ(componentsWithNoLet->size(), componentsWithLet->size());
}

}  // namespace
}  // namespace mongo::query_shape
