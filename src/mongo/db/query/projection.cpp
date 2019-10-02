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

#include "mongo/db/query/projection.h"

#include "mongo/base/exact_cast.h"
#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/projection_ast_walker.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace projection_ast {
namespace {
/**
 * Holds data used for dependency analysis while walking an AST tree. This struct is attached to
 * 'PathTrackingVisitorContext' and can be accessed by projection AST visitors to track the current
 * context.
 */
struct DepsAnalysisData {
    DepsTracker fieldDependencyTracker{DepsTracker::kAllMetadataAvailable};

    void addRequiredField(const std::string& fieldName) {
        fieldDependencyTracker.fields.insert(fieldName);
    }

    std::vector<std::string> requiredFields() const {
        return {fieldDependencyTracker.fields.begin(), fieldDependencyTracker.fields.end()};
    }
};

/**
 * Does "broad" analysis on the projection, about whether the entire document, or details from the
 * match expression are needed and so on.
 */
class ProjectionAnalysisVisitor final : public ProjectionASTVisitor {
public:
    ProjectionAnalysisVisitor(ProjectionDependencies* deps) : _deps(deps) {
        invariant(_deps);
    }

    void visit(ProjectionPathASTNode* node) final {
        if (node->parent()) {
            _deps->hasDottedPath = true;
        }
    }

    void visit(ProjectionPositionalASTNode* node) final {
        _deps->requiresMatchDetails = true;
        _deps->requiresDocument = true;
    }

    void visit(ProjectionSliceASTNode* node) final {
        _deps->requiresDocument = true;
    }

    void visit(ProjectionElemMatchASTNode* node) final {
        _deps->requiresDocument = true;
    }

    void visit(ExpressionASTNode* node) final {
        const Expression* expr = node->expressionRaw();
        const ExpressionMeta* meta = dynamic_cast<const ExpressionMeta*>(expr);

        // Only {$meta: 'sortKey'} projections can be covered. Projections with any other expression
        // need the document.
        if (!(meta && meta->getMetaType() == DocumentMetadataFields::MetaType::kSortKey)) {
            _deps->requiresDocument = true;
        }
    }

    void visit(BooleanConstantASTNode* node) final {}
    void visit(MatchExpressionASTNode* node) final {}

private:
    ProjectionDependencies* _deps;
};

/**
 * Uses a DepsTracker to determine which fields are required from the projection.
 *
 * To track the current path in the projection, this visitor should be used with
 * 'PathTrackingWalker' which will help to maintain the current path via
 * 'PathTrackingVisitorContext'.
 */
class DepsAnalysisVisitor final : public ProjectionASTVisitor {
public:
    DepsAnalysisVisitor(PathTrackingVisitorContext<DepsAnalysisData>* context) : _context{context} {
        invariant(_context);
    }

    void visit(MatchExpressionASTNode* node) final {
        node->matchExpression()->addDependencies(&_context->data().fieldDependencyTracker);
    }

    void visit(ProjectionPositionalASTNode* node) final {
        // Positional projection on a.b.c.$ may actually modify a, a.b, a.b.c, etc.
        // Treat the top-level field as a dependency.
        addTopLevelPathAsDependency();
    }

    void visit(ProjectionSliceASTNode* node) final {
        // find() $slice on a.b.c may modify a, a.b, and a.b.c if they're all arrays.
        // Treat the top-level field as a dependency.
        addTopLevelPathAsDependency();
    }

    void visit(ProjectionElemMatchASTNode* node) final {
        addFullPathAsDependency();
    }

    void visit(ExpressionASTNode* node) final {
        // The output of an expression on a dotted path depends on whether that field is an array.
        invariant(node->parent());
        if (!node->parent()->isRoot()) {
            addFullPathAsDependency();
        }

        node->expression()->addDependencies(&_context->data().fieldDependencyTracker);
    }

    void visit(BooleanConstantASTNode* node) final {
        // For inclusions, we depend on the field.
        if (node->value()) {
            addFullPathAsDependency();
        }
    }

    void visit(ProjectionPathASTNode* node) final {}

private:
    void addTopLevelPathAsDependency() {
        const auto& path = _context->fullPath();

        _context->data().addRequiredField(path.front().toString());
    }

    void addFullPathAsDependency() {
        const auto& path = _context->fullPath();

        _context->data().addRequiredField(path.fullPath());
    }

    PathTrackingVisitorContext<DepsAnalysisData>* _context;
};

auto analyzeProjection(ProjectionPathASTNode* root, ProjectType type) {
    ProjectionDependencies deps;
    PathTrackingVisitorContext<DepsAnalysisData> context;
    DepsAnalysisVisitor depsAnalysisVisitor{&context};
    ProjectionAnalysisVisitor projectionAnalysisVisitor{&deps};
    PathTrackingWalker walker{&context, {&depsAnalysisVisitor, &projectionAnalysisVisitor}, {}};

    projection_ast_walker::walk(&walker, root);

    const auto& userData = context.data();
    const auto& tracker = userData.fieldDependencyTracker;

    if (type == ProjectType::kInclusion) {
        deps.requiredFields = userData.requiredFields();
    } else {
        invariant(type == ProjectType::kExclusion);
        deps.requiresDocument = true;
    }

    deps.needsTextScore = tracker.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE);
    deps.needsGeoPoint = tracker.getNeedsMetadata(DepsTracker::MetadataType::GEO_NEAR_POINT);
    deps.needsGeoDistance = tracker.getNeedsMetadata(DepsTracker::MetadataType::GEO_NEAR_DISTANCE);
    deps.needsSortKey = tracker.getNeedsMetadata(DepsTracker::MetadataType::SORT_KEY);

    return deps;
}
}  // namespace


Projection::Projection(ProjectionPathASTNode root, ProjectType type, const BSONObj& bson)
    : _root(std::move(root)), _type(type), _deps(analyzeProjection(&_root, type)), _bson(bson) {}

namespace {

/**
 * Given an AST node for a projection and a path, return the node representing the deepest
 * common point between the path and the tree, as well as the index into the path following that
 * node.
 *
 * Example:
 * Node representing tree {a: {b: 1, c: {d: 1}}}
 * path: "a.b"
 * Returns: inclusion node for {b: 1} and index 2.
 *
 * Node representing tree {a: {b: 0, c: 0}}
 * path: "a.b.c.d"
 * Returns: exclusion node for {c: 0} and index 3.
 */
std::pair<const ASTNode*, size_t> findCommonPoint(const ASTNode* astNode,
                                                  const FieldPath& path,
                                                  size_t pathIndex) {
    if (pathIndex >= path.getPathLength()) {
        // We've run out of path. That is, the projection goes deeper than the path requested.
        // For example, the projection may be {a.b : 1} and the requested field might be 'a'.
        return {astNode, path.getPathLength()};
    }

    const auto* pathNode = exact_pointer_cast<const ProjectionPathASTNode*>(astNode);
    if (pathNode) {
        // We can look up children.
        StringData field = path.getFieldName(pathIndex);
        const auto* child = pathNode->getChild(field);

        if (!child) {
            // This node is the common point.
            return {astNode, pathIndex};
        }

        return findCommonPoint(child, path, pathIndex + 1);
    }

    // This is a terminal node with respect to the projection. We can't traverse any more, so
    // return the current node.
    return {astNode, pathIndex};
}
}  // namespace

bool Projection::isFieldRetainedExactly(StringData path) {
    FieldPath fieldPath(path);

    const auto [node, pathIndex] = findCommonPoint(&_root, fieldPath, 0);

    // Check the type of the node. If it's a 'path' node then we know more
    // inclusions/exclusions are beneath it.
    if (const auto* pathNode = exact_pointer_cast<const ProjectionPathASTNode*>(node)) {
        // There are two cases:
        // (I) we project a subfield of the requested path. E.g. the projection is
        // {a.b.c: <value>} and the requested path was 'a.b'. In this case, the field is not
        // necessarily retained exactly.
        if (pathIndex == fieldPath.getPathLength()) {
            return false;
        }

        // (II) We project a 'sibling' field of the requested path. E.g. the projection is
        // {a.b.x: <value>} and the requested path is 'a.b.c'. The common point would be at 'a.b'.
        // In this case, the field is retained exactly if the projection is an exclusion.
        if (pathIndex < fieldPath.getPathLength()) {
            invariant(!pathNode->getChild(fieldPath.getFieldName(pathIndex)));
            return _type == ProjectType::kExclusion;
        }

        MONGO_UNREACHABLE;
    } else if (const auto* boolNode = exact_pointer_cast<const BooleanConstantASTNode*>(node)) {
        // If the node is an inclusion, then the path is preserved.
        // This is true even if the path is deeper than the AST, e.g. if the projection is
        // {a.b: 1} and the requested field is 'a.b.c.
        return boolNode->value();
    }

    return false;
}
}  // namespace projection_ast
}  // namespace mongo
