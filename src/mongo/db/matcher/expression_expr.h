/**
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

#include <vector>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/rewrite_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

/**
 * MatchExpression for the top-level $expr keyword. Take an expression as an argument, evaluates and
 * coerces to boolean form which determines whether a document is a match.
 */
class ExprMatchExpression final : public MatchExpression {
public:
    ExprMatchExpression(BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final {
        MONGO_UNREACHABLE;
    }

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void debugString(StringBuilder& debug, int level = 0) const final {
        _debugAddSpace(debug, level);
        debug << "$expr " << _expression->serialize(false).toString();
    }

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    void _doSetCollator(const CollatorInterface* collator) final;

    void _doAddDependencies(DepsTracker* deps) const final {
        if (_expression) {
            _expression->addDependencies(deps);
        }
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    boost::intrusive_ptr<Expression> _expression;

    boost::optional<RewriteExpr::RewriteResult> _rewriteResult;
};

}  // namespace mongo
