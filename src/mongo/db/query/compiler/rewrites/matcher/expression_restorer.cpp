// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/matcher/expression_restorer.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {
namespace {
using boolean_simplification::BitsetTerm;
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::Minterm;
class MatchExpressionRestorer {
public:
    MatchExpressionRestorer(const BitsetTreeNode& root,
                            const BitsetTreeTransformResult::ExpressionList& expressions)
        : _root(root), _expressions(expressions) {}

    std::unique_ptr<MatchExpression> restore() const {
        if (_root.type == BitsetTreeNode::Or && _root.internalChildren.empty() &&
            _root.leafChildren.mask.none()) {
            return std::make_unique<AlwaysFalseMatchExpression>();
        }
        return restore(_root);
    }

private:
    std::unique_ptr<MatchExpression> restore(const BitsetTreeNode& node) const {
        tassert(8163020,
                "BitsetTreeNode must be non-negative to be restored to MatchExpression",
                !node.isNegated);

        std::vector<std::unique_ptr<MatchExpression>> children{};
        for (size_t bitIndex = node.leafChildren.mask.findFirst();
             bitIndex < node.leafChildren.mask.size();
             bitIndex = node.leafChildren.mask.findNext(bitIndex)) {
            children.emplace_back(restoreOneLeaf(node.leafChildren, bitIndex));
        }

        for (const auto& child : node.internalChildren) {
            children.emplace_back(restore(child));
        }

        if (children.size() == 1) {
            return std::move(children.front());
        }

        std::unique_ptr<MatchExpression> expr{};
        switch (node.type) {
            case BitsetTreeNode::And:
                return std::make_unique<AndMatchExpression>(std::move(children));
            case BitsetTreeNode::Or:
                return std::make_unique<OrMatchExpression>(std::move(children));
            default:
                MONGO_UNREACHABLE_TASSERT(7767003);
        }
    }

    std::unique_ptr<MatchExpression> restoreOneLeaf(const BitsetTerm& term, size_t bitIndex) const {
        if (!term.predicates[bitIndex]) {
            return std::make_unique<NotMatchExpression>(_expressions[bitIndex].expression->clone());
        }

        return _expressions[bitIndex].expression->clone();
    }

    const BitsetTreeNode& _root;
    const BitsetTreeTransformResult::ExpressionList& _expressions;
};
}  // namespace

std::unique_ptr<MatchExpression> restoreMatchExpression(
    const boolean_simplification::BitsetTreeNode& bitsetTree,
    const BitsetTreeTransformResult::ExpressionList& expressions) {
    MatchExpressionRestorer restorer(bitsetTree, expressions);
    return restorer.restore();
}
}  // namespace mongo
