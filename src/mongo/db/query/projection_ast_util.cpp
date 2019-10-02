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

#include "mongo/db/query/projection_ast_util.h"

#include "mongo/db/query/projection_ast_walker.h"

namespace mongo::projection_ast {
namespace {
struct BSONVisitorContext {
    std::stack<std::list<std::string>> fieldNames;
    std::stack<BSONObjBuilder> builders;

    BSONObjBuilder& builder() {
        return builders.top();
    }
};

class BSONPreVisitor : public ProjectionASTVisitor {
public:
    BSONPreVisitor(BSONVisitorContext* context) : _context(context) {}

    virtual void visit(MatchExpressionASTNode* node) {
        static_cast<const MatchExpressionASTNode*>(node)->matchExpression()->serialize(
            &_context->builder());
        _context->fieldNames.top().pop_front();
    }
    virtual void visit(ProjectionPathASTNode* node) {
        if (!node->parent()) {
            // No root of the tree, thus this node has no field name.
            _context->builders.push(BSONObjBuilder());
        } else {
            _context->builders.push(_context->builder().subobjStart(getFieldName()));
        }

        // Push all of the field names onto a new layer on the field name stack.
        _context->fieldNames.push(
            std::list<std::string>(node->fieldNames().begin(), node->fieldNames().end()));
    }
    virtual void visit(ProjectionPositionalASTNode* node) {
        // ProjectionPositional always has the original query's match expression node as its
        // child. Serialize as: {"positional.projection.field.$": <original match expression>}.
        _context->builders.push(_context->builder().subobjStart(getFieldName() + ".$"));
        // Since match expressions serialize their own field name, this is not actually used. It's
        // pushed since when a MatchExpressionASTNode is visited it expects a field name to have
        // been put on the stack (just like every other node), and will pop it.
        _context->fieldNames.push({"<dummy>"});
    }
    virtual void visit(ProjectionSliceASTNode* node) {
        BSONObjBuilder sub(_context->builder().subobjStart(getFieldName()));
        if (node->skip()) {
            sub.appendArray("$slice", BSON_ARRAY(*node->skip() << node->limit()));
        } else {
            sub.appendNumber("$slice", node->limit());
        }
    }
    virtual void visit(ProjectionElemMatchASTNode* node) {
        // Defer to the child, match expression node.
    }
    virtual void visit(ExpressionASTNode* node) {
        node->expression()->serialize(false).addToBsonObj(&_context->builder(), getFieldName());
    }
    virtual void visit(BooleanConstantASTNode* node) {
        _context->builders.top().append(getFieldName(), node->value());
    }

private:
    std::string getFieldName() {
        invariant(!_context->fieldNames.empty());
        invariant(!_context->fieldNames.top().empty());
        auto ret = _context->fieldNames.top().front();
        _context->fieldNames.top().pop_front();
        return ret;
    }

    BSONVisitorContext* _context;
};

class BSONPostVisitor : public ProjectionASTVisitor {
public:
    BSONPostVisitor(BSONVisitorContext* context) : _context(context) {}

    virtual void visit(MatchExpressionASTNode* node) {}
    virtual void visit(ProjectionPathASTNode* node) {
        // Don't pop the top builder.
        if (node->parent()) {
            // Pop the BSONObjBuilder that was added in the pre visitor.
            _context->builders.pop();
        }

        // Make sure all of the children were serialized.
        invariant(_context->fieldNames.top().empty());
        _context->fieldNames.pop();
    }
    virtual void visit(ProjectionPositionalASTNode* node) {
        _context->builders.pop();
        _context->fieldNames.pop();
    }

    virtual void visit(ProjectionSliceASTNode* node) {}
    virtual void visit(ProjectionElemMatchASTNode* node) {}
    virtual void visit(ExpressionASTNode* node) {}
    virtual void visit(BooleanConstantASTNode* node) {}

private:
    BSONVisitorContext* _context;
};

class BSONWalker {
public:
    BSONWalker() : _preVisitor(&_context), _postVisitor(&_context) {}

    void preVisit(ASTNode* node) {
        node->acceptVisitor(&_preVisitor);
    }

    void postVisit(ASTNode* node) {
        node->acceptVisitor(&_postVisitor);
    }

    void inVisit(long count, ASTNode* node) {
        // No op.
    }

    BSONObjBuilder done() {
        invariant(_context.fieldNames.empty());
        invariant(_context.builders.size() == 1);

        auto ret = std::move(_context.builders.top());
        return ret;
    }

private:
    BSONVisitorContext _context;
    BSONPreVisitor _preVisitor;
    BSONPostVisitor _postVisitor;
};
}  // namespace

BSONObj astToDebugBSON(ASTNode* root) {
    BSONWalker walker;
    projection_ast_walker::walk(&walker, root);

    BSONObjBuilder bob = walker.done();
    return bob.obj();
}
}  // namespace mongo::projection_ast
