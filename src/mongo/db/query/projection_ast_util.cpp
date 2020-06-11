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

#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/tree_walker.h"

namespace mongo::projection_ast {
namespace {
struct BSONVisitorContext {
    std::stack<BSONObjBuilder> builders;
};

class BSONPreVisitor : public ProjectionASTConstVisitor {
public:
    BSONPreVisitor(PathTrackingVisitorContext<BSONVisitorContext>* context)
        : _context(context), _builders(context->data().builders) {}

    virtual void visit(const MatchExpressionASTNode* node) {
        static_cast<const MatchExpressionASTNode*>(node)->matchExpression()->serialize(
            &_builders.top(), true);
    }

    virtual void visit(const ProjectionPathASTNode* node) {
        if (!node->parent()) {
            // No root of the tree, thus this node has no field name.
            _builders.push(BSONObjBuilder());
        } else {
            _builders.push(_builders.top().subobjStart(getFieldName()));
        }
    }

    virtual void visit(const ProjectionPositionalASTNode* node) {
        // ProjectionPositional always has the original query's match expression node as its
        // child. Serialize as: {"positional.projection.field.$": <original match expression>}.
        _context->data().builders.push(_builders.top().subobjStart(getFieldName() + ".$"));
    }

    virtual void visit(const ProjectionSliceASTNode* node) {
        BSONObjBuilder sub(_builders.top().subobjStart(getFieldName()));
        if (node->skip()) {
            sub.appendArray("$slice", BSON_ARRAY(*node->skip() << node->limit()));
        } else {
            sub.appendNumber("$slice", node->limit());
        }
    }

    virtual void visit(const ProjectionElemMatchASTNode* node) {
        // Defer to the child, match expression node.
    }

    virtual void visit(const ExpressionASTNode* node) {
        node->expression()->serialize(false).addToBsonObj(&_builders.top(), getFieldName());
    }

    virtual void visit(const BooleanConstantASTNode* node) {
        _builders.top().append(getFieldName(), node->value());
    }

private:
    std::string getFieldName() {
        return _context->childPath();
    }

    PathTrackingVisitorContext<BSONVisitorContext>* _context;
    std::stack<BSONObjBuilder>& _builders;
};

class BSONPostVisitor : public ProjectionASTConstVisitor {
public:
    BSONPostVisitor(BSONVisitorContext* context) : _context(context) {}

    virtual void visit(const ProjectionPathASTNode* node) {
        // Don't pop the top builder.
        if (node->parent()) {
            // Pop the BSONObjBuilder that was added in the pre visitor.
            _context->builders.pop();
        }
    }

    virtual void visit(const ProjectionPositionalASTNode* node) {
        _context->builders.pop();
    }

    virtual void visit(const MatchExpressionASTNode* node) {}
    virtual void visit(const ProjectionSliceASTNode* node) {}
    virtual void visit(const ProjectionElemMatchASTNode* node) {}
    virtual void visit(const ExpressionASTNode* node) {}
    virtual void visit(const BooleanConstantASTNode* node) {}

private:
    BSONVisitorContext* _context;
};
}  // namespace

BSONObj astToDebugBSON(const ASTNode* root) {
    PathTrackingVisitorContext<BSONVisitorContext> context;
    BSONPreVisitor preVisitor{&context};
    BSONPostVisitor postVisitor{&context.data()};
    PathTrackingWalker walker{&context, {&preVisitor}, {&postVisitor}};

    tree_walker::walk<true, projection_ast::ASTNode>(root, &walker);

    invariant(context.data().builders.size() == 1);
    return context.data().builders.top().obj();
}
}  // namespace mongo::projection_ast
