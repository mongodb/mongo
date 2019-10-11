/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/exact_cast.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/parsed_exclusion_projection.h"
#include "mongo/db/pipeline/parsed_inclusion_projection.h"
#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo::projection_executor {
namespace {
using ParsedAggregationProjection = parsed_aggregation_projection::ParsedAggregationProjection;
using ParsedInclusionProjection = parsed_aggregation_projection::ParsedInclusionProjection;
using ParsedExclusionProjection = parsed_aggregation_projection::ParsedExclusionProjection;

constexpr auto kInclusion = projection_ast::ProjectType::kInclusion;
constexpr auto kExclusion = projection_ast::ProjectType::kExclusion;
constexpr auto kProjectionPostImageVarName =
    parsed_aggregation_projection::ParsedAggregationProjection::kProjectionPostImageVarName;

/**
 * Holds data used to built a projection executor while walking an AST tree. This struct is attached
 * to 'PathTrackingVisitorContext' and can be accessed by projection AST visitors to track the
 * current context.
 */
template <typename Executor>
struct ProjectionExecutorVisitorData {
    // A projection executor returned upon completion of AST traversal.
    std::unique_ptr<Executor> executor;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    // A root replacement expression to be attached to the projection executor, if any. If there
    // are multiple root replacement expressions in the AST, they will be chained together, so that
    // one expression becomes an input to another.
    boost::intrusive_ptr<Expression> rootReplacementExpression;

    auto rootNode() const {
        return executor->getRoot();
    }

    void setRootReplacementExpression(boost::intrusive_ptr<Expression> expr) {
        rootReplacementExpression = expr;
        executor->setRootReplacementExpression(rootReplacementExpression);
    }
};

template <typename Executor>
using ProjectionExecutorVisitorContext =
    projection_ast::PathTrackingVisitorContext<ProjectionExecutorVisitorData<Executor>>;

template <typename Executor>
auto makeProjectionPreImageExpression(const ProjectionExecutorVisitorData<Executor>& data) {
    return ExpressionFieldPath::parse(data.expCtx, "$$ROOT", data.expCtx->variablesParseState);
}

template <typename Executor>
auto makeProjectionPostImageExpression(const ProjectionExecutorVisitorData<Executor>& data) {
    return data.rootReplacementExpression
        ? data.rootReplacementExpression
        : ExpressionFieldPath::parse(
              data.expCtx, "$$" + kProjectionPostImageVarName, data.expCtx->variablesParseState);
}

/**
 * Creates a find()-style positional expression from the given AST 'node' to be applied to the
 * 'path' on the input document. If the visitor 'data' already contains a root replacement
 * expression, it will be used as an input operand to the new root replacement expression, otherwise
 * a field path expressions will be created to access a projection post-image document.
 */
template <typename Executor>
auto createFindPositionalExpression(const projection_ast::ProjectionPositionalASTNode* node,
                                    const ProjectionExecutorVisitorData<Executor>& data,
                                    const FieldPath& path) {
    invariant(node);

    const auto& children = node->children();
    invariant(children.size() == 1UL);

    auto matchExprNode =
        exact_pointer_cast<projection_ast::MatchExpressionASTNode*>(children[0].get());
    invariant(matchExprNode);

    return make_intrusive<ExpressionInternalFindPositional>(data.expCtx,
                                                            makeProjectionPreImageExpression(data),
                                                            makeProjectionPostImageExpression(data),
                                                            path,
                                                            matchExprNode->matchExpression());
}

/**
 * Creates a find()-style $slice expression from the given AST 'node' to be applied to the
 * 'path' on the input document. If the visitor 'data' already contains a root replacement
 * expression, it will be used as an input operand to the new root replacement expression, otherwise
 * a field path expressions will be created to access a projection post-image document.
 */
template <typename Executor>
auto createFindSliceExpression(const projection_ast::ProjectionSliceASTNode* node,
                               const ProjectionExecutorVisitorData<Executor>& data,
                               const FieldPath& path) {
    invariant(node);

    return make_intrusive<ExpressionInternalFindSlice>(
        data.expCtx, makeProjectionPostImageExpression(data), path, node->skip(), node->limit());
}

/**
 * Creates a find()-style $elemMatch expression from the given AST 'node' to be applied at the
 * 'path' on the input document.
 */
template <typename Executor>
auto createFindElemMatchExpression(const projection_ast::ProjectionElemMatchASTNode* node,
                                   const ProjectionExecutorVisitorData<Executor>& data,
                                   const FieldPath& path) {
    invariant(node);

    const auto& children = node->children();
    invariant(children.size() == 1UL);

    auto matchExprNode =
        exact_pointer_cast<projection_ast::MatchExpressionASTNode*>(children[0].get());
    invariant(matchExprNode);

    return make_intrusive<ExpressionInternalFindElemMatch>(data.expCtx,
                                                           makeProjectionPreImageExpression(data),
                                                           path,
                                                           matchExprNode->matchExpression());
}

/**
 * A projection AST visitor which creates inclusion or exclusion nodes in the projection execution
 * tree by calling corresponding 'addProjectionForPath()' or 'addExpressionForPath()' on the root
 * node of the tree while traversing the AST. If the AST contains root-replacement expressions, they
 * will be attached to the projection executor.
 *
 * To track the current path in the projection, this visitor should be used with
 * 'PathTrackingWalker' which will help to maintain the current path via
 * 'PathTrackingVisitorContext'.
 */
template <typename Executor>
class ProjectionExecutorVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionExecutorVisitor(ProjectionExecutorVisitorContext<Executor>* context)
        : _context{context} {
        invariant(_context);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        constexpr auto isInclusion = std::is_same_v<Executor, ParsedInclusionProjection>;
        invariant(isInclusion);

        const auto& path = _context->fullPath();
        auto& userData = _context->data();

        userData.rootNode()->addProjectionForPath(path.fullPath());
        userData.setRootReplacementExpression(createFindPositionalExpression(node, userData, path));
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        const auto& path = _context->fullPath();
        auto& userData = _context->data();

        // A $slice expression can be applied to an exclusion projection. In this case we don't need
        // to project out the path to which $slice is applied, since it will already be included
        // into the output document.
        if constexpr (std::is_same_v<Executor, ParsedInclusionProjection>) {
            userData.rootNode()->addProjectionForPath(path.fullPath());
        }

        userData.setRootReplacementExpression(createFindSliceExpression(node, userData, path));
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        const auto& path = _context->fullPath();
        const auto& userData = _context->data();

        userData.rootNode()->addExpressionForPath(
            path.fullPath(), createFindElemMatchExpression(node, userData, path));
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        const auto& path = _context->fullPath();
        const auto& userData = _context->data();

        userData.rootNode()->addExpressionForPath(path.fullPath(), node->expression());
    }

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        const auto& path = _context->fullPath();
        const auto& userData = _context->data();

        // In an inclusion projection only the _id field can be excluded from the result document.
        // If this is the case, then we don't need to include the field into the projection.
        if constexpr (std::is_same_v<Executor, ParsedInclusionProjection>) {
            const auto isIdField = path == "_id";
            if (isIdField && !node->value()) {
                return;
            }
            // In inclusion projection only _id field can be excluded, make sure this is the case.
            invariant(!isIdField || node->value());
        }

        userData.rootNode()->addProjectionForPath(path.fullPath());
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {}
    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    ProjectionExecutorVisitorContext<Executor>* _context;
};

/**
 * A helper function which creates a 'ProjectionExecutorWalker' to walk a projection AST,
 * starting at the node 'root', and build a projection executor of the specified type
 * 'Executor'.
 */
template <typename Executor>
auto buildProjectionExecutor(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const projection_ast::ProjectionPathASTNode* root,
                             const ProjectionPolicies policies) {
    ProjectionExecutorVisitorContext<Executor> context{
        {std::make_unique<Executor>(expCtx, policies), expCtx}};
    ProjectionExecutorVisitor<Executor> executorVisitor{&context};
    projection_ast::PathTrackingWalker walker{&context, {&executorVisitor}, {}};
    projection_ast_walker::walk(&walker, root);
    return std::move(context.data().executor);
}
}  // namespace

std::unique_ptr<ParsedAggregationProjection> buildProjectionExecutor(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const projection_ast::Projection* projection,
    const ProjectionPolicies policies) {
    invariant(projection);

    switch (projection->type()) {
        case kInclusion:
            return buildProjectionExecutor<ParsedInclusionProjection>(
                expCtx, projection->root(), policies);
        case kExclusion:
            return buildProjectionExecutor<ParsedExclusionProjection>(
                expCtx, projection->root(), policies);
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace mongo::projection_executor
