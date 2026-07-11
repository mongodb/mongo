// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class RewriteExpr final {
public:
    /**
     * Holds the result of an Expression rewrite operation. $expr expressions can't take advantage
     * of indexes. When we rewrite the expressions as a conjunction of internal match expressions,
     * the query planner can now use the internal match expressions to potentially generate an index
     * scan. We use internal match expressions that are non-type bracketed to match non-type
     * bracketed comparison operators inside $expr.
     */
    class RewriteResult final {
    public:
        RewriteResult(std::unique_ptr<MatchExpression> matchExpression,
                      bool allSubExpressionsRewritten)
            : _matchExpression(std::move(matchExpression)),
              _allSubExpressionsRewritten(allSubExpressionsRewritten) {}

        MatchExpression* matchExpression() const {
            return _matchExpression.get();
        }

        std::unique_ptr<MatchExpression> releaseMatchExpression() {
            return std::move(_matchExpression);
        }

        RewriteResult clone() const {
            auto clonedMatch = _matchExpression ? _matchExpression->clone() : nullptr;
            return {std::move(clonedMatch), _allSubExpressionsRewritten};
        }

        bool allSubExpressionsRewritten() {
            return _allSubExpressionsRewritten;
        }

    private:
        std::unique_ptr<MatchExpression> _matchExpression;

        // Defaults to true, is false if there is a child in an $or/$and expression that contains
        // children that cannot be rewritten to a MatchExpression.
        bool _allSubExpressionsRewritten = true;
    };

    /**
     * Attempts to construct a MatchExpression that will match against either an identical set or a
     * superset of the documents matched by 'expr'. Due to semantic differences the rewritten
     * MatchExpression might match more documents than the ExprMatchExpression. For example,
     * $_internalExprEq in MatchExpression reaches into arrays, and $eq in ExprMatchExpression does
     * not. However, $_internalExprLt/$_internalExprGt are non-type bracketed for MatchExpression,
     * just like ExprMatchExpressions. Returns the MatchExpression as a RewriteResult. If a rewrite
     * is not possible, RewriteResult::matchExpression() will return a nullptr.
     */
    static RewriteResult rewrite(const boost::intrusive_ptr<Expression>& expr,
                                 const CollatorInterface* collator);

private:
    RewriteExpr(const CollatorInterface* collator) : _collator(collator) {}

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteExpression(
        const boost::intrusive_ptr<Expression>& currExprNode);

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteAndExpression(
        const boost::intrusive_ptr<ExpressionAnd>& currExprNode);

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteOrExpression(
        const boost::intrusive_ptr<ExpressionOr>& currExprNode);

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteComparisonExpression(
        const boost::intrusive_ptr<ExpressionCompare>& expr);

    bool _canRewriteComparison(const boost::intrusive_ptr<ExpressionCompare>& expr) const;

    std::unique_ptr<MatchExpression> _buildComparisonMatchExpression(
        ExpressionCompare::CmpOp comparisonOp, BSONElement fieldAndValue);

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteInExpression(
        const boost::intrusive_ptr<ExpressionIn>& expr);

    const CollatorInterface* _collator;
    bool _allSubExpressionsRewritten = true;
};

}  // namespace mongo
