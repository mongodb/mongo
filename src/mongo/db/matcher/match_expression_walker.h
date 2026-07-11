// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * A match expression tree walker compatible with tree_walker::walk() to be used with const
 * MatchExpression visitors.
 */
class MatchExpressionWalker final {
public:
    MatchExpressionWalker(MatchExpressionConstVisitor* preVisitor,
                          MatchExpressionConstVisitor* inVisitor,
                          MatchExpressionConstVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(const MatchExpression* expr) {
        if (_preVisitor != nullptr) {
            expr->acceptVisitor(_preVisitor);
        }
    }

    void postVisit(const MatchExpression* expr) {
        if (_postVisitor != nullptr) {
            expr->acceptVisitor(_postVisitor);
        }
    }

    void inVisit(long count, const MatchExpression* expr) {
        if (_inVisitor != nullptr) {
            expr->acceptVisitor(_inVisitor);
        }
    }

private:
    MatchExpressionConstVisitor* _preVisitor;
    MatchExpressionConstVisitor* _inVisitor;
    MatchExpressionConstVisitor* _postVisitor;
};
}  // namespace mongo
