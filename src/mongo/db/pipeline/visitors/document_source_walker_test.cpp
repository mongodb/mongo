/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/visitors/document_source_walker.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
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
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")), expCtx);

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
    auto pipeline = Pipeline::parse(makeVector(fromjson("{$match: {a: 1}}"),
                                               fromjson("{$project: {_id: 0}}"),
                                               fromjson("{$project: {b: 2}}")),
                                    expCtx);

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
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")), expCtx);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    walker.walk(*pipeline);

    std::vector<std::string> expected = {"match", "project"};
    ASSERT_EQ(expected, ctx.seen);
}

DEATH_TEST_REGEX(DocumentSourceWalker, UnimplementedStage, "Tripwire assertion.*6202701") {
    DocumentSourceVisitorRegistry reg;
    // Register match and not project.
    registerVisitFuncs<VisitorCtxImpl, DocumentSourceMatch>(&reg);
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$match: {a: 1}}"), fromjson("{$project: {_id: 0}}")), expCtx);

    VisitorCtxImpl ctx;
    DocumentSourceWalker walker(reg, &ctx);
    ASSERT_THROWS_CODE(walker.walk(*pipeline), DBException, 6202701);
}

DEATH_TEST_REGEX(DocumentSourceWalker, DuplicateRegistryKey, "Tripwire assertion.*6202700") {
    auto f = []() {
        DocumentSourceVisitorRegistry reg;
        registerVisitFuncs<VisitorCtxImpl, DocumentSourceMatch, DocumentSourceMatch>(&reg);
    };
    ASSERT_THROWS_CODE(f(), DBException, 6202700);
}

}  // namespace mongo
