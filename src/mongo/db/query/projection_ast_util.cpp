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

#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/query/tree_walker.h"

namespace mongo::projection_ast {
namespace {
struct BSONVisitorContext {
    std::stack<BSONObjBuilder> builders;
    bool underElemMatch = false;
};

class BSONPreVisitor : public ProjectionASTConstVisitor {
public:
    using ProjectionASTConstVisitor::visit;
    BSONPreVisitor(PathTrackingVisitorContext<BSONVisitorContext>* context,
                   const SerializationOptions& options)
        : _context(context), _builders(context->data().builders), _options(options) {}


    void visit(const ProjectionPathASTNode* node) override {
        if (!node->parent()) {
            // No root of the tree, thus this node has no field name.
            _builders.push(BSONObjBuilder());
        } else {
            _builders.push(_builders.top().subobjStart(getFieldName()));
        }
    }

    void visit(const ProjectionSliceASTNode* node) override {
        BSONObjBuilder sub(_builders.top().subobjStart(getFieldName()));
        if (node->skip()) {
            if (_options.replacementForLiteralArgs) {
                const auto rep = _options.replacementForLiteralArgs.value();
                sub.appendArray("$slice", BSON_ARRAY(rep << rep));
            } else {
                sub.appendArray("$slice", BSON_ARRAY(*node->skip() << node->limit()));
            }
        } else {
            if (_options.replacementForLiteralArgs) {
                sub.append("$slice", _options.replacementForLiteralArgs.value());
            } else {
                sub.appendNumber("$slice", node->limit());
            }
        }
    }


    void visit(const ExpressionASTNode* node) override {
        node->expression()->serialize(_options).addToBsonObj(&_builders.top(), getFieldName());
    }

    void visit(const BooleanConstantASTNode* node) override {
        _builders.top().append(getFieldName(), node->value());
    }

    void visit(const ProjectionPositionalASTNode* node) override = 0;
    void visit(const ProjectionElemMatchASTNode* node) override = 0;
    void visit(const MatchExpressionASTNode* node) override = 0;

protected:
    std::string getFieldName() {
        return _options.serializeFieldPathFromString(_context->childPath());
    }

    PathTrackingVisitorContext<BSONVisitorContext>* _context;
    std::stack<BSONObjBuilder>& _builders;
    SerializationOptions _options;
};

class BSONPostVisitor : public ProjectionASTConstVisitor {
public:
    using ProjectionASTConstVisitor::visit;
    BSONPostVisitor(BSONVisitorContext* context) : _context(context) {}

    void visit(const ProjectionPathASTNode* node) override {
        // Don't pop the top builder.
        if (node->parent()) {
            // Pop the BSONObjBuilder that was added in the pre visitor.
            _context->builders.pop();
        }
    }

    void visit(const ProjectionSliceASTNode* node) override {}
    void visit(const ExpressionASTNode* node) override {}
    void visit(const BooleanConstantASTNode* node) override {}
    void visit(const MatchExpressionASTNode* node) override {}

    void visit(const ProjectionPositionalASTNode* node) override = 0;
    void visit(const ProjectionElemMatchASTNode* node) override = 0;

protected:
    BSONVisitorContext* _context;
};

class DebugPreVisitor : public BSONPreVisitor {
public:
    using BSONPreVisitor::visit;
    DebugPreVisitor(PathTrackingVisitorContext<BSONVisitorContext>* context)
        : BSONPreVisitor(context, SerializationOptions{}) {}

    void visit(const ProjectionPositionalASTNode* node) override {
        // ProjectionPositional always has the original query's match expression node as its
        // child. Serialize as: {"positional.projection.field.$": <original match expression>}.
        _context->data().builders.push(_builders.top().subobjStart(getFieldName() + ".$"));
    }

    void visit(const ProjectionElemMatchASTNode* node) override {
        // Defer to the child, match expression node.
    }

    void visit(const MatchExpressionASTNode* node) override {
        static_cast<const MatchExpressionASTNode*>(node)->matchExpression()->serialize(
            &_builders.top(), {});
    }
};

class DebugPostVisitor : public BSONPostVisitor {
public:
    using BSONPostVisitor::visit;
    DebugPostVisitor(BSONVisitorContext* context) : BSONPostVisitor(context) {}

    void visit(const ProjectionPositionalASTNode* node) override {
        _context->builders.pop();
    }

    void visit(const ProjectionElemMatchASTNode* node) override {}
};

class SerializationPreVisitor : public BSONPreVisitor {
public:
    using BSONPreVisitor::visit;
    SerializationPreVisitor(PathTrackingVisitorContext<BSONVisitorContext>* context,
                            SerializationOptions options)
        : BSONPreVisitor(context, options) {}

    void visit(const ProjectionPositionalASTNode* node) override {
        tassert(73488,
                "Positional projection should not appear below an $elemMatch projection.",
                !_context->data().underElemMatch);
        _builders.top().append(getFieldName() + ".$", true);
    }

    void visit(const ProjectionElemMatchASTNode* node) override {
        // The child match expression node should begin with $elemMatch.
        _context->data().underElemMatch = true;
    }

    void visit(const MatchExpressionASTNode* node) override {
        if (_context->data().underElemMatch) {
            static_cast<const MatchExpressionASTNode*>(node)->matchExpression()->serialize(
                &_builders.top(), _options);
        }
    }
};

class SerializationPostVisitor : public BSONPostVisitor {
public:
    using BSONPostVisitor::visit;
    SerializationPostVisitor(BSONVisitorContext* context) : BSONPostVisitor(context) {}

    void visit(const ProjectionPositionalASTNode* node) override {}
    void visit(const ProjectionElemMatchASTNode* node) override {
        _context->underElemMatch = false;
    }
};

}  // namespace

BSONObj astToDebugBSON(const ASTNode* root) {
    PathTrackingVisitorContext<BSONVisitorContext> context;
    DebugPreVisitor preVisitor{&context};
    DebugPostVisitor postVisitor{&context.data()};
    PathTrackingWalker walker{&context, {&preVisitor}, {&postVisitor}};

    tree_walker::walk<true, projection_ast::ASTNode>(root, &walker);

    invariant(context.data().builders.size() == 1);
    return context.data().builders.top().obj();
}

BSONObj serialize(const Projection& ast, SerializationOptions options) {
    PathTrackingVisitorContext<BSONVisitorContext> context;
    SerializationPreVisitor preVisitor{&context, options};
    SerializationPostVisitor postVisitor{&context.data()};
    PathTrackingWalker walker{&context, {&preVisitor}, {&postVisitor}};

    tree_walker::walk<true, projection_ast::ASTNode>(ast.root(), &walker);

    invariant(context.data().builders.size() == 1);
    return context.data().builders.top().obj();
}
}  // namespace mongo::projection_ast
