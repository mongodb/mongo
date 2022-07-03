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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/rewrite_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * MatchExpression for the top-level $expr keyword. Take an expression as an argument, evaluates and
 * coerces to boolean form which determines whether a document is a match.
 */
class ExprMatchExpression final : public MatchExpression {
public:
    ExprMatchExpression(BSONElement elem,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final {
        MONGO_UNREACHABLE;
    }

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    /**
     * Evaluates the aggregation expression of this match expression on document 'doc' and returns
     * the result.
     */
    Value evaluateExpression(const MatchableDocument* doc) const;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << "$expr " << _expression->serialize(false).toString();
    }

    void serialize(BSONObjBuilder* out, bool includePath) const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400207);
    }

    void resetChild(size_t, MatchExpression*) {
        MONGO_UNREACHABLE;
    };

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    boost::intrusive_ptr<ExpressionContext> getExpressionContext() const {
        return _expCtx;
    }

    boost::intrusive_ptr<Expression> getExpression() const {
        return _expression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the
     * expression. Each pair in 'renameList' specifies a path prefix that should be renamed (as the
     * first element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        SubstituteFieldPathWalker substituteWalker(renameList);
        expression_walker::walk<Expression>(_expression.get(), &substituteWalker);
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
