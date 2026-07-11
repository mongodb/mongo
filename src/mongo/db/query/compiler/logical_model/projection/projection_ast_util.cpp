// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"

#include <stack>
#include <string>
#include <string_view>
#include <vector>

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
                   query_shape::SerializationOptions options)
        : _context(context), _builders(context->data().builders), _options(std::move(options)) {}

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
            sub.appendArray("$slice",
                            BSON_ARRAY(_options.serializeLiteral(*node->skip())
                                       << _options.serializeLiteral(node->limit())));
        } else {
            _options.appendLiteral(&sub, "$slice", node->limit());
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
    query_shape::SerializationOptions _options;
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
        : BSONPreVisitor(context, query_shape::SerializationOptions{}) {}

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
                            const query_shape::SerializationOptions& options)
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
            const auto& me = node->matchExpression();

            // When a projection's $elemMatch starts with a "$", PathMatchExpression::serialize
            // wraps the $-prefixed path in "$_internalPath", producing a shape that is not valid
            // projection syntax and cannot be re-parsed for querystats. Therefore we have to
            // construct a re-parseable projection: "{$<path>: {$elemMatch: <rhs of equality>}}".
            const std::string_view path = me->path();
            if (path.starts_with('$') && _options.isSerializingForQueryStats()) {
                BSONObjBuilder fieldSub(
                    _builders.top().subobjStart(_options.serializeFieldPathFromString(path)));
                BSONObjBuilder elemMatchSub(fieldSub.subobjStart("$elemMatch"));
                me->serialize(&elemMatchSub, _options, /*includePath*/ false);
            } else {
                me->serialize(&_builders.top(), _options);
            }
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

    tassert(11051944,
            "Stack of BSONObjBuilders doesn't contain a single element",
            context.data().builders.size() == 1);
    return context.data().builders.top().obj();
}

BSONObj serialize(const ProjectionPathASTNode& root,
                  const query_shape::SerializationOptions& options) {
    PathTrackingVisitorContext<BSONVisitorContext> context;
    SerializationPreVisitor preVisitor{&context, options};
    SerializationPostVisitor postVisitor{&context.data()};
    PathTrackingWalker walker{&context, {&preVisitor}, {&postVisitor}};
    tree_walker::walk<true, projection_ast::ASTNode>(&root, &walker);

    tassert(11051943,
            "Stack of BSONObjBuilders doesn't contain a single element",
            context.data().builders.size() == 1);
    return context.data().builders.top().obj();
}
}  // namespace mongo::projection_ast
