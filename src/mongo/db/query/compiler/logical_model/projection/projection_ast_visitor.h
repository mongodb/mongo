// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/tree_walker.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace projection_ast {
class ASTNode;
class MatchExpressionASTNode;
class ProjectionPathASTNode;
class ProjectionPositionalASTNode;
class ProjectionSliceASTNode;
class ProjectionElemMatchASTNode;
class ExpressionASTNode;
class BooleanConstantASTNode;

/**
 * Visitor pattern for ProjectionAST.
 *
 * This code is not responsible for traversing the AST, only for performing the double-dispatch.
 *
 * If the visitor doesn't intend to modify the AST, then the template argument 'IsConst' should be
 * set to 'true'. In this case all 'visit()' methods will take a const pointer to a visiting node.
 */
template <bool IsConst = false>
class ProjectionASTVisitor {
public:
    virtual ~ProjectionASTVisitor() = default;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, MatchExpressionASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionPathASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionPositionalASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionSliceASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionElemMatchASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ExpressionASTNode> node) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, BooleanConstantASTNode> node) = 0;
};

using ProjectionASTMutableVisitor = ProjectionASTVisitor<false>;
using ProjectionASTConstVisitor = ProjectionASTVisitor<true>;

template <bool IsConst = false>
class ProjectionASTWalker {
public:
    using VisitorPtr = ProjectionASTVisitor<IsConst>*;
    using ASTNodePtr = tree_walker::MaybeConstPtr<IsConst, ASTNode>;

    ProjectionASTWalker(VisitorPtr preVisitor, VisitorPtr inVisitor, VisitorPtr postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(ASTNodePtr node) {
        if (_preVisitor)
            node->acceptVisitor(_preVisitor);
    }

    void postVisit(ASTNodePtr node) {
        if (_postVisitor)
            node->acceptVisitor(_postVisitor);
    }

    void inVisit(long count, ASTNodePtr node) {
        if (_inVisitor)
            node->acceptVisitor(_inVisitor);
    }

private:
    VisitorPtr _preVisitor;
    VisitorPtr _inVisitor;
    VisitorPtr _postVisitor;
};

using ProjectionASTMutableWalker = ProjectionASTWalker<false>;
using ProjectionASTConstWalker = ProjectionASTWalker<true>;
}  // namespace projection_ast
}  // namespace mongo
