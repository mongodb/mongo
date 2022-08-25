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

#include "mongo/db/pipeline/abt/transformer_visitor.h"

#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/visitors/transformer_interface_walker.h"

namespace mongo::optimizer {

void ABTTransformerVisitor::visit(
    const projection_executor::AddFieldsProjectionExecutor* transformer) {
    visitInclusionNode(transformer->getRoot(), true /*isAddingFields*/);
}

void ABTTransformerVisitor::visit(
    const projection_executor::ExclusionProjectionExecutor* transformer) {
    visitExclusionNode(*transformer->getRoot());
}

void ABTTransformerVisitor::visit(
    const projection_executor::InclusionProjectionExecutor* transformer) {
    visitInclusionNode(*transformer->getRoot(), false /*isAddingFields*/);
}

void ABTTransformerVisitor::visit(const GroupFromFirstDocumentTransformation* transformer) {
    unsupportedTransformer(transformer);
}

void ABTTransformerVisitor::visit(const ReplaceRootTransformation* transformer) {
    auto entry = _ctx.getNode();
    const std::string& projName = _ctx.getNextId("newRoot");
    ABT expr =
        generateAggExpression(transformer->getExpression().get(), entry._rootProjection, projName);

    _ctx.setNode<EvaluationNode>(projName, projName, std::move(expr), std::move(entry._node));
}

void ABTTransformerVisitor::generateCombinedProjection() const {
    auto result = _builder.generateABT();
    if (!result) {
        return;
    }

    auto entry = _ctx.getNode();
    const ProjectionName projName = _ctx.getNextId("combinedProjection");
    _ctx.setNode<EvaluationNode>(projName, projName, std::move(*result), std::move(entry._node));
}

void ABTTransformerVisitor::unsupportedTransformer(const TransformerInterface* transformer) const {
    uasserted(ErrorCodes::InternalErrorNotSupported,
              str::stream() << "Transformer is not supported (code: "
                            << static_cast<int>(transformer->getType()) << ")");
}

void ABTTransformerVisitor::assertSupportedPath(const std::string& path) {
    uassert(ErrorCodes::InternalErrorNotSupported,
            "Projection contains unsupported numeric path component",
            !FieldRef(path).hasNumericPathComponents());
}

/**
 * Handles simple inclusion projections.
 */
void ABTTransformerVisitor::processProjectedPaths(const projection_executor::InclusionNode& node) {
    // For each preserved path, mark that each path element along the field path should be
    // included.
    OrderedPathSet preservedPaths;
    node.reportProjectedPaths(&preservedPaths);

    for (const std::string& preservedPathStr : preservedPaths) {
        assertSupportedPath(preservedPathStr);

        _builder.integrateFieldPath(FieldPath(preservedPathStr),
                                    [](const bool isLastElement, FieldMapEntry& entry) {
                                        entry._hasLeadingObj = true;
                                        entry._hasKeep = true;
                                    });
    }
}

/**
 * Handles renamed fields and computed projections.
 */
void ABTTransformerVisitor::processComputedPaths(const projection_executor::InclusionNode& node,
                                                 const std::string& rootProjection,
                                                 const bool isAddingFields) {
    OrderedPathSet computedPaths;
    StringMap<std::string> renamedPaths;
    node.reportComputedPaths(&computedPaths, &renamedPaths);

    // Handle path renames: essentially single element FieldPath expression.
    for (const auto& renamedPathEntry : renamedPaths) {
        ABT path = translateFieldPath(
            FieldPath(renamedPathEntry.second),
            make<PathIdentity>(),
            [](const std::string& fieldName, const bool isLastElement, ABT input) {
                return make<PathGet>(
                    fieldName,
                    isLastElement ? std::move(input)
                                  : make<PathTraverse>(std::move(input), PathTraverse::kUnlimited));
            });

        auto entry = _ctx.getNode();
        const std::string& renamedProjName = _ctx.getNextId("projRenamedPath");
        _ctx.setNode<EvaluationNode>(
            entry._rootProjection,
            renamedProjName,
            make<EvalPath>(std::move(path), make<Variable>(entry._rootProjection)),
            std::move(entry._node));

        _builder.integrateFieldPath(
            FieldPath(renamedPathEntry.first),
            [&renamedProjName, &isAddingFields](const bool isLastElement, FieldMapEntry& entry) {
                if (!isAddingFields) {
                    entry._hasKeep = true;
                }
                if (isLastElement) {
                    entry._constVarName = renamedProjName;
                    entry._hasTrailingDefault = true;
                }
            });
    }

    // Handle general expression projection.
    for (const std::string& computedPathStr : computedPaths) {
        assertSupportedPath(computedPathStr);

        const FieldPath computedPath(computedPathStr);

        auto entry = _ctx.getNode();
        const std::string& getProjName = _ctx.getNextId("projGetPath");
        ABT getExpr = generateAggExpression(
            node.getExpressionForPath(computedPath).get(), rootProjection, getProjName);

        _ctx.setNode<EvaluationNode>(std::move(entry._rootProjection),
                                     getProjName,
                                     std::move(getExpr),
                                     std::move(entry._node));

        _builder.integrateFieldPath(
            computedPath,
            [&getProjName, &isAddingFields](const bool isLastElement, FieldMapEntry& entry) {
                if (!isAddingFields) {
                    entry._hasKeep = true;
                }
                if (isLastElement) {
                    entry._constVarName = getProjName;
                    entry._hasTrailingDefault = true;
                }
            });
    }
}

void ABTTransformerVisitor::visitInclusionNode(const projection_executor::InclusionNode& node,
                                               const bool isAddingFields) {
    auto entry = _ctx.getNode();
    const std::string rootProjection = entry._rootProjection;

    processProjectedPaths(node);
    processComputedPaths(node, rootProjection, isAddingFields);
}

void ABTTransformerVisitor::visitExclusionNode(const projection_executor::ExclusionNode& node) {
    // Handle simple exclusion projections: for each excluded path, mark that the last field
    // path element should be dropped.
    OrderedPathSet excludedPaths;
    node.reportProjectedPaths(&excludedPaths);
    for (const std::string& excludedPathStr : excludedPaths) {
        assertSupportedPath(excludedPathStr);
        _builder.integrateFieldPath(FieldPath(excludedPathStr),
                                    [](const bool isLastElement, FieldMapEntry& entry) {
                                        if (isLastElement) {
                                            entry._hasDrop = true;
                                        }
                                    });
    }
}

/**
 * TODO SERVER-68690: Refactor this function to be shared with the DocumentSource implementation.
 */
void translateProjection(AlgebrizerContext& ctx,
                         ProjectionName scanProjName,
                         boost::intrusive_ptr<ExpressionContext> expCtx,
                         const projection_ast::Projection* proj) {

    const auto projExecutor = projection_executor::buildProjectionExecutor(
        expCtx,
        proj,
        ProjectionPolicies::findProjectionPolicies(),
        projection_executor::BuilderParamsBitSet{projection_executor::kDefaultBuilderParams});

    FieldMapBuilder builder(scanProjName, true);
    ABTTransformerVisitor visitor(ctx, builder);
    TransformerInterfaceWalker walker(&visitor);
    walker.walk(projExecutor.get());
    visitor.generateCombinedProjection();
}

}  // namespace mongo::optimizer
