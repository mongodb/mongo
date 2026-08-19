// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_reordering.h"

#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace {
/**
 * Visitor that enables predicate reordering on every AND/OR/NOR node it visits.
 */
struct MatchExpressionAllowReorderingVisitor : public SelectiveMatchExpressionVisitorBase<false> {
    using SelectiveMatchExpressionVisitorBase<false>::visit;

    void visit(AndMatchExpression* expr) final {
        expr->allowReordering();
    }
    void visit(OrMatchExpression* expr) final {
        expr->allowReordering();
    }
    void visit(NorMatchExpression* expr) final {
        expr->allowReordering();
    }
};

/**
 * Tree walker adapter that invokes a 'MatchExpressionAllowReorderingVisitor' on every node in
 * pre-order, so that a single pass enables reordering throughout the entire expression tree.
 */
class MatchExpressionAllowReorderingWalker {
public:
    explicit MatchExpressionAllowReorderingWalker(MatchExpressionAllowReorderingVisitor* visitor)
        : _visitor{visitor} {}

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}
    void inVisit(long count, MatchExpression* expr) {}

private:
    MatchExpressionAllowReorderingVisitor* _visitor;
};

}  // namespace

void allowReordering(OperationContext* opCtx, MatchExpression* expr) {
    if (!expr || !opCtx ||
        !QueryKnobConfiguration::get(opCtx).getEnableChangeStreamMatchExpressionReordering()) {
        return;
    }

    MatchExpressionAllowReorderingVisitor visitor;
    MatchExpressionAllowReorderingWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(expr, &walker);
}

}  // namespace mongo
