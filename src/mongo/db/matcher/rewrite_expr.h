/*-
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

class RewriteExpr final {
public:
    /**
     * Holds the result of an Expression rewrite operation.
     */
    class RewriteResult final {
    public:
        RewriteResult(std::unique_ptr<MatchExpression> matchExpression,
                      std::vector<std::string> matchExprStringStorage,
                      std::vector<BSONObj> matchExprElemStorage)
            : _matchExpression(std::move(matchExpression)),
              _matchExprStringStorage(std::move(matchExprStringStorage)),
              _matchExprElemStorage(std::move(matchExprElemStorage)) {}

        MatchExpression* matchExpression() const {
            return _matchExpression.get();
        }

        std::unique_ptr<MatchExpression> releaseMatchExpression() {
            return std::move(_matchExpression);
        }

    private:
        std::unique_ptr<MatchExpression> _matchExpression;

        // MatchExpression nodes are constructed with BSONElement and StringData arguments that are
        // externally owned and expected to outlive the MatchExpression. '_matchExprStringStorage'
        // and '_matchExprElemStorage' hold the underlying std::string and BSONObj storage for these
        // arguments.
        std::vector<std::string> _matchExprStringStorage;
        std::vector<BSONObj> _matchExprElemStorage;
    };

    /**
     * Attempts to construct a MatchExpression that will match against either an identical set or a
     * superset of the documents matched by 'expr'. Returns the MatchExpression as a RewriteResult.
     * If a rewrite is not possible, RewriteResult::matchExpression() will return a nullptr.
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
    std::unique_ptr<MatchExpression> _rewriteInExpression(
        const boost::intrusive_ptr<ExpressionIn>& currExprNode);

    // Returns rewritten MatchExpression or null unique_ptr if not rewritable.
    std::unique_ptr<MatchExpression> _rewriteComparisonExpression(
        const boost::intrusive_ptr<ExpressionCompare>& currExprNode);

    bool _isValidMatchComparison(const boost::intrusive_ptr<ExpressionCompare>& expr) const;

    bool _isValidMatchIn(const boost::intrusive_ptr<ExpressionIn>& expr) const;

    bool _isValidFieldPath(const FieldPath& fieldPath) const;

    std::unique_ptr<MatchExpression> _buildComparisonMatchExpression(
        const boost::intrusive_ptr<ExpressionCompare>& expr);

    std::unique_ptr<MatchExpression> _buildComparisonMatchExpression(
        ExpressionCompare::CmpOp comparisonOp, BSONElement fieldAndValue);

    std::vector<BSONObj> _matchExprElemStorage;
    std::vector<std::string> _matchExprStringStorage;
    const CollatorInterface* _collator;
};

}  // namespace mongo
