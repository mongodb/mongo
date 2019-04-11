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

#pragma once

#include "mongo/platform/basic.h"

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
