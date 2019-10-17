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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/base/exact_cast.h"
#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/projection_ast_walker.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

void QueryPlannerCommon::reverseScans(QuerySolutionNode* node) {
    StageType type = node->getType();

    if (STAGE_IXSCAN == type) {
        IndexScanNode* isn = static_cast<IndexScanNode*>(node);
        isn->direction *= -1;

        isn->bounds = isn->bounds.reverse();

        invariant(isn->bounds.isValidFor(isn->index.keyPattern, isn->direction),
                  str::stream() << "Invalid bounds: " << redact(isn->bounds.toString()));

        // TODO: we can just negate every value in the already computed properties.
        isn->computeProperties();
    } else if (STAGE_DISTINCT_SCAN == type) {
        DistinctNode* dn = static_cast<DistinctNode*>(node);
        dn->direction *= -1;

        dn->bounds = dn->bounds.reverse();

        invariant(dn->bounds.isValidFor(dn->index.keyPattern, dn->direction),
                  str::stream() << "Invalid bounds: " << redact(dn->bounds.toString()));

        dn->computeProperties();
    } else if (STAGE_SORT_MERGE == type) {
        // reverse direction of comparison for merge
        MergeSortNode* msn = static_cast<MergeSortNode*>(node);
        msn->sort = reverseSortObj(msn->sort);
    } else {
        invariant(STAGE_SORT != type);
        // This shouldn't be here...
    }

    for (size_t i = 0; i < node->children.size(); ++i) {
        reverseScans(node->children[i]);
    }
}

namespace {

struct MetaFieldData {
    std::vector<FieldPath> metaPaths;
};

using MetaFieldVisitorContext = projection_ast::PathTrackingVisitorContext<MetaFieldData>;

/**
 * Visitor which produces a list of paths where $meta expressions are.
 */
class MetaFieldVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    MetaFieldVisitor(MetaFieldVisitorContext* context) : _context(context) {}


    void visit(const projection_ast::ExpressionASTNode* node) final {
        const auto* metaExpr = exact_pointer_cast<const ExpressionMeta*>(node->expressionRaw());
        if (!metaExpr || metaExpr->getMetaType() != DocumentMetadataFields::MetaType::kSortKey) {
            return;
        }

        _context->data().metaPaths.push_back(_context->fullPath());
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {}
    void visit(const projection_ast::ProjectionSliceASTNode* node) final {}
    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {}
    void visit(const projection_ast::BooleanConstantASTNode* node) final {}
    void visit(const projection_ast::ProjectionPathASTNode* node) final {}
    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    MetaFieldVisitorContext* _context;
};
}  // namespace

std::vector<FieldPath> QueryPlannerCommon::extractSortKeyMetaFieldsFromProjection(
    const projection_ast::Projection& proj) {

    MetaFieldVisitorContext ctx;
    MetaFieldVisitor visitor(&ctx);
    projection_ast::PathTrackingConstWalker<MetaFieldData> walker{&ctx, {&visitor}, {}};
    projection_ast_walker::walk(&walker, proj.root());

    return std::move(ctx.data().metaPaths);
}
}  // namespace mongo
