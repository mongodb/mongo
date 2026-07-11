// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/util/modules.h"

namespace mongo::stage_builder {
class ExpressionWalker final {
public:
    ExpressionWalker(ExpressionConstVisitor* preVisitor,
                     ExpressionConstVisitor* inVisitor,
                     ExpressionConstVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(const Expression* expr) {
        if (_preVisitor) {
            expr->acceptVisitor(_preVisitor);
        }
    }

    void inVisit(long long count, const Expression* expr) {
        if (_inVisitor) {
            expr->acceptVisitor(_inVisitor);
        }
    }

    void postVisit(const Expression* expr) {
        if (_postVisitor) {
            expr->acceptVisitor(_postVisitor);
        }
    }

private:
    ExpressionConstVisitor* _preVisitor;
    ExpressionConstVisitor* _inVisitor;
    ExpressionConstVisitor* _postVisitor;
};
}  // namespace mongo::stage_builder
