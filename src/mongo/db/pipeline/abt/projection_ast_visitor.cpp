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

#include "mongo/db/pipeline/abt/projection_ast_visitor.h"

#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/projection_ast_path_tracking_visitor.h"

namespace mongo::optimizer {

class ProjectionPreVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    ProjectionPreVisitor(projection_ast::PathTrackingVisitorContext<>* context,
                         bool isInclusion,
                         const ProjectionName& rootProjName,
                         const ProjectionName& scanProjName)
        : _context{context},
          _builder(rootProjName, rootProjName == scanProjName),
          _isInclusion(isInclusion) {
        invariant(_context);
    }

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        unsupportedProjectionType("ProjectionPositionalASTNode");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        unsupportedProjectionType("ProjectionSliceASTNode");
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        unsupportedProjectionType("ProjectionElemMatchASTNode");
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        unsupportedProjectionType("ExpressionASTNode");
    }

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        const auto& path = _context->fullPath();
        if (_isInclusion) {
            const auto isIdField = path == "_id";
            // If current field is _id and _id : 0, then don't include it.
            if (isIdField && !node->value()) {
                return;
            }
            // In inclusion projection only _id field can be excluded, make sure this is the case.
            tassert(
                6684601, "In inclusion projection only _id field can be excluded", node->value());
            builderIntegrateInclusion(path.fullPath());
        } else {
            builderIntegrateExclusion(path.fullPath());
        }
    }

    void visit(const projection_ast::ProjectionPathASTNode* node) final {}
    void visit(const projection_ast::MatchExpressionASTNode* node) final {
        unsupportedProjectionType("MatchExpressionASTNode");
    }

    auto generateABT() {
        return _builder.generateABT();
    }

private:
    projection_ast::PathTrackingVisitorContext<>* _context;
    FieldMapBuilder _builder;
    bool _isInclusion;

    void assertSupportedPath(const std::string& path) {
        uassert(ErrorCodes::InternalErrorNotSupported,
                "Projection contains unsupported numeric path component",
                !FieldRef(path).hasNumericPathComponents());
    }

    void builderIntegrateInclusion(const std::string& fullPath) {
        assertSupportedPath(fullPath);
        _builder.integrateFieldPath(FieldPath(fullPath),
                                    [](const bool isLastElement, FieldMapEntry& entry) {
                                        entry._hasLeadingObj = true;
                                        entry._hasKeep = true;
                                    });
    }

    void builderIntegrateExclusion(const std::string& fullPath) {
        assertSupportedPath(fullPath);
        _builder.integrateFieldPath(FieldPath(fullPath),
                                    [](const bool isLastElement, FieldMapEntry& entry) {
                                        if (isLastElement) {
                                            entry._hasDrop = true;
                                        }
                                    });
    }

    void unsupportedProjectionType(const std::string& unsupportedNode) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Projection node is not supported (type: " << unsupportedNode
                                << ")");
    }
};

void translateProjection(AlgebrizerContext& ctx, const projection_ast::Projection& proj) {
    projection_ast::PathTrackingVisitorContext context{};
    const bool isInclusion = proj.type() == projection_ast::ProjectType::kInclusion;
    const ProjectionName& rootProjName = ctx.getNode()._rootProjection;

    ProjectionPreVisitor astVisitor{&context, isInclusion, rootProjName, ctx.getScanProjName()};
    projection_ast::PathTrackingWalker walker{&context, {&astVisitor}, {}};
    tree_walker::walk<true, projection_ast::ASTNode>(proj.root(), &walker);

    auto result = astVisitor.generateABT();
    tassert(7021702, "Failed to generate ABT for projection", result);

    auto entry = ctx.getNode();
    const ProjectionName projName = ctx.getNextId("combinedProjection");
    ctx.setNode<EvaluationNode>(projName, projName, std::move(*result), std::move(entry._node));
}

}  // namespace mongo::optimizer
