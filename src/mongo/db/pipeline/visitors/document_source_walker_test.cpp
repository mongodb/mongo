// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/visitors/document_source_walker.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

// Dummy visitor context with visit functions that keep track of the stage visited.
struct VisitorCtxImpl : public DocumentSourceVisitorContextBase {
    std::vector<std::string> seen;
};

void visit(VisitorCtxImpl* ctx, const DocumentSourceMatch&) {
    ctx->seen.push_back("match");
}

void visit(VisitorCtxImpl* ctx, const DocumentSourceSingleDocumentTransformation&) {
    ctx->seen.push_back("project");
}

struct VisitorCtxNoop : public DocumentSourceVisitorContextBase {};
void visit(VisitorCtxNoop* ctx, const DocumentSourceMatch&) {}
void visit(VisitorCtxNoop* ctx, const DocumentSourceSingleDocumentTransformation&) {}

TEST(DocumentSourceWalker, RegisterAndUseStages) {
    DocumentSourceVisitorRegistry reg;
    registerVisitFuncs<VisitorCtxImpl,
                       DocumentSourceMatch,
                       DocumentSourceSingleDocumentTransformation>(&reg);

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")),
        expCtx,
        pipeline_factory::kOptionsMinimal);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    walker.walk(*pipeline);

    std::vector<std::string> expected = {"match", "project"};
    ASSERT_EQ(expected, ctx.seen);
}

TEST(DocumentSourceWalker, WalkerMultipleDuplicateStages) {
    DocumentSourceVisitorRegistry reg;
    registerVisitFuncs<VisitorCtxImpl,
                       DocumentSourceMatch,
                       DocumentSourceSingleDocumentTransformation>(&reg);

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto pipeline = pipeline_factory::makePipeline(makeVector(fromjson("{$match: {a: 1}}"),
                                                              fromjson("{$project: {_id: 0}}"),
                                                              fromjson("{$project: {b: 2}}")),
                                                   expCtx,
                                                   pipeline_factory::kOptionsMinimal);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    walker.walk(*pipeline);

    std::vector<std::string> expected = {"match", "project", "project"};
    ASSERT_EQ(expected, ctx.seen);
}

TEST(DocumentSourceWalker, MultipleVisitorsRegistered) {
    DocumentSourceVisitorRegistry reg;
    registerVisitFuncs<VisitorCtxImpl,
                       DocumentSourceMatch,
                       DocumentSourceSingleDocumentTransformation>(&reg);
    registerVisitFuncs<VisitorCtxNoop,
                       DocumentSourceMatch,
                       DocumentSourceSingleDocumentTransformation>(&reg);

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")),
        expCtx,
        pipeline_factory::kOptionsMinimal);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    walker.walk(*pipeline);

    std::vector<std::string> expected = {"match", "project"};
    ASSERT_EQ(expected, ctx.seen);
}

DEATH_TEST_REGEX(DocumentSourceWalkerDeathTest, UnimplementedStage, "Tripwire assertion.*6202701") {
    DocumentSourceVisitorRegistry reg;
    // Register match and not project.
    registerVisitFuncs<VisitorCtxImpl, DocumentSourceMatch>(&reg);
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")),
        expCtx,
        pipeline_factory::kOptionsMinimal);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    ASSERT_THROWS_CODE(walker.walk(*pipeline), DBException, 6202701);
}

DEATH_TEST_REGEX(DocumentSourceWalkerDeathTest,
                 DuplicateRegistryKey,
                 "Tripwire assertion.*6202700") {
    auto f = []() {
        DocumentSourceVisitorRegistry reg;
        registerVisitFuncs<VisitorCtxImpl, DocumentSourceMatch, DocumentSourceMatch>(&reg);
    };
    ASSERT_THROWS_CODE(f(), DBException, 6202700);
}

}  // namespace mongo
