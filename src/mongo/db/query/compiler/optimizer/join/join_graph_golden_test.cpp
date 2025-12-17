/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {
class JoinGraphGoldenTest : public unittest::Test {
public:
    JoinGraphGoldenTest() : _cfg{"src/mongo/db/test_output/query/compiler/optimizer/join"} {
        _opCtx = _serviceContext.makeOperationContext();
    }

    void runVariation(MutableJoinGraph mgraph, StringData variationName) {
        JoinGraph graph(std::move(mgraph));
        unittest::GoldenTestContext ctx(&_cfg);
        ctx.outStream() << "VARIATION " << variationName << std::endl;
        ctx.outStream() << "output: " << graph.toString(/*pretty*/ true) << std::endl;
        ctx.outStream() << std::endl;
    }

    std::unique_ptr<CanonicalQuery> makeCanonicalQuery(BSONObj filter) {
        auto cmd = BSON("find" << "a" << "filter" << filter << "$db" << "test");
        auto findCommand = query_request_helper::makeFromFindCommandForTests(cmd);
        auto expCtx = ExpressionContextBuilder{}.fromRequest(_opCtx.get(), *findCommand).build();
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = std::move(expCtx),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)},
        });
    }

private:
    unittest::GoldenTestConfig _cfg;
    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(JoinGraphGoldenTest, buildGraph) {
    MutableJoinGraph graph{};

    auto a = *graph.addNode(makeNSS("a"), makeCanonicalQuery(BSON("a" << 1)), boost::none);
    auto b = *graph.addNode(makeNSS("b"), makeCanonicalQuery(BSON("b" << 1)), FieldPath("b"));
    auto c = *graph.addNode(makeNSS("c"), makeCanonicalQuery(BSON("c" << 1)), FieldPath("c"));
    auto d = *graph.addNode(makeNSS("d"), nullptr, FieldPath("d"));

    graph.addSimpleEqualityEdge(a, b, 0, 1);
    graph.addSimpleEqualityEdge(a, c, 2, 3);
    graph.addSimpleEqualityEdge(c, d, 4, 5);

    runVariation(std::move(graph), "buildGraph");
}
}  // namespace mongo::join_ordering
