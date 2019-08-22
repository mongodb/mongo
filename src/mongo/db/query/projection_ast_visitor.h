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

namespace mongo {
namespace projection_ast {

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
 */
class ProjectionASTVisitor {
public:
    virtual ~ProjectionASTVisitor() = default;
    virtual void visit(MatchExpressionASTNode* node) = 0;
    virtual void visit(ProjectionPathASTNode* node) = 0;
    virtual void visit(ProjectionPositionalASTNode* node) = 0;
    virtual void visit(ProjectionSliceASTNode* node) = 0;
    virtual void visit(ProjectionElemMatchASTNode* node) = 0;
    virtual void visit(ExpressionASTNode* node) = 0;
    virtual void visit(BooleanConstantASTNode* node) = 0;
};
}  // namespace projection_ast
}  // namespace mongo
