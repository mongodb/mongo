// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::join_ordering {
class JoinGraphGoldenTest : public unittest::Test {
public:
    JoinGraphGoldenTest() : _cfg{"src/mongo/db/test_output/query/compiler/optimizer/join"} {
        _opCtx = _serviceContext.makeOperationContext();
    }

    void runVariation(MutableJoinGraph mgraph, std::string_view variationName) {
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
