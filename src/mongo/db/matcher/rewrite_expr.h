/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/collation/collator_interface.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

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
                      std::vector<BSONObj> matchExprElemStorage,
                      bool allSubExpressionsRewritten)
            : _matchExpression(std::move(matchExpression)),
              _matchExprElemStorage(std::move(matchExprElemStorage)),
              _allSubExpressionsRewritten(allSubExpressionsRewritten) {}

        MatchExpression* matchExpression() const {
            return _matchExpression.get();
        }

        std::unique_ptr<MatchExpression> releaseMatchExpression() {
            return std::move(_matchExpression);
        }

        RewriteResult clone() const {
            auto clonedMatch = _matchExpression ? _matchExpression->clone() : nullptr;
            return {std::move(clonedMatch), _matchExprElemStorage, _allSubExpressionsRewritten};
        }

        bool allSubExpressionsRewritten() {
            return _allSubExpressionsRewritten;
        }

    private:
        std::unique_ptr<MatchExpression> _matchExpression;

        // MatchExpression nodes are constructed with BSONElement arguments that are externally
        // owned and expected to outlive the MatchExpression. '_matchExprElemStorage' holds the
        // underlying BSONObj storage for these arguments.
        std::vector<BSONObj> _matchExprElemStorage;

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

    std::vector<BSONObj> _matchExprElemStorage;
    const CollatorInterface* _collator;
    bool _allSubExpressionsRewritten = true;
};

}  // namespace mongo
