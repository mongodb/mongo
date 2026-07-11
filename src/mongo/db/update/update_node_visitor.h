// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"


namespace mongo {

class AddToSetNode;
class ArithmeticNode;
class BitNode;
class CompareNode;
class ConflictPlaceholderNode;
class CurrentDateNode;
class PopNode;
class PullAllNode;
class PullNode;
class PushNode;
class RenameNode;
class SetElementNode;
class SetNode;
class UnsetNode;
class UpdateArrayNode;
class UpdateObjectNode;

/**
 * This is a base class to allow for traversal of an update tree. It implements the visitor
 * pattern, in which every derived class from update node implements an acceptVisitor() method,
 * which simply calls the appropriate visit() method on the derived UpdateNodeVisitor class. The
 * derived class can do whatever it needs to do for each specific node type in the corresponding
 * visit() method.
 * Derived classes are responsible for making the recursive calls to visit() if they wish
 * to visit all the nodes in the update node tree. UpdateNodeVisitor's purpose is not actually to
 * ensure that every node in the tree is visited, but rather to handle dynamic dispatch without
 * having to add virtual methods to the UpdateNode interface itself.
 */
class UpdateNodeVisitor {
public:
    virtual ~UpdateNodeVisitor() = default;
    virtual void visit(AddToSetNode*) = 0;
    virtual void visit(ArithmeticNode*) = 0;
    virtual void visit(BitNode*) = 0;
    virtual void visit(CompareNode*) = 0;
    virtual void visit(ConflictPlaceholderNode*) = 0;
    virtual void visit(CurrentDateNode*) = 0;
    virtual void visit(PopNode*) = 0;
    virtual void visit(PullAllNode*) = 0;
    virtual void visit(PullNode*) = 0;
    virtual void visit(PushNode*) = 0;
    virtual void visit(RenameNode*) = 0;
    virtual void visit(SetElementNode*) = 0;
    virtual void visit(SetNode*) = 0;
    virtual void visit(UnsetNode*) = 0;
    virtual void visit(UpdateArrayNode*) = 0;
    virtual void visit(UpdateObjectNode*) = 0;
};
}  // namespace mongo
