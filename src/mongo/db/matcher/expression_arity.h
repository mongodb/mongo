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

#include <algorithm>
#include <array>
#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/matcher/expression.h"

namespace mongo {

/**
 * Abstract base class for MatchExpressions that take a fixed 'nargs' number of MatchExpression
 * arguments. 'T' is the MatchExpression class that extends this interface.
 */
template <typename T, size_t nargs>
class FixedArityMatchExpression : public MatchExpression {
public:
    /**
     * The number of arguments accepted by this expression.
     */
    static constexpr size_t arity() {
        return nargs;
    }

    virtual ~FixedArityMatchExpression() = default;

    void debugString(StringBuilder& debug, int indentationLevel) const final {
        _debugAddSpace(debug, indentationLevel);

        BSONObjBuilder builder;
        serialize(&builder, true);
        debug << builder.obj().toString();
    }

    /**
     * The order of arguments is significant when determining equivalence.
     */
    bool equivalent(const MatchExpression* expr) const final {
        if (matchType() != expr->matchType()) {
            return false;
        }

        auto other = static_cast<const T*>(expr);
        const auto& ourChildren = _expressions;
        const auto& theirChildren = other->_expressions;
        return std::equal(
            ourChildren.begin(),
            ourChildren.end(),
            theirChildren.begin(),
            theirChildren.end(),
            [](const auto& expr1, const auto& expr2) { return expr1->equivalent(expr2.get()); });
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return nargs;
    }

    MatchExpression* getChild(size_t i) const final {
        tassert(6400203, "Out-of-bounds access to child of MatchExpression.", i < nargs);
        return _expressions[i].get();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329406, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _expressions[i].reset(other);
    }

    /**
     * The name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    /**
     * Serializes each subexpression sequentially in a BSONArray.
     */
    void serialize(BSONObjBuilder* builder, bool includePath) const final {
        BSONArrayBuilder exprArray(builder->subarrayStart(name()));
        for (const auto& expr : _expressions) {
            BSONObjBuilder exprBuilder(exprArray.subobjStart());
            expr->serialize(&exprBuilder, includePath);
            exprBuilder.doneFast();
        }
        exprArray.doneFast();
    }

    /**
     * Clones this MatchExpression by recursively cloning each sub-expression.
     */
    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::array<std::unique_ptr<MatchExpression>, nargs> clonedExpressions;
        std::transform(_expressions.begin(),
                       _expressions.end(),
                       clonedExpressions.begin(),
                       [](const auto& orig) {
                           return orig ? orig->shallowClone()
                                       : std::unique_ptr<MatchExpression>(nullptr);
                       });
        std::unique_ptr<T> clone =
            std::make_unique<T>(std::move(clonedExpressions), _errorAnnotation);

        if (getTag()) {
            clone->setTag(getTag()->clone());
        }

        return clone;
    }

protected:
    /**
     * Takes ownership of the MatchExpressions in 'expressions'.
     */
    explicit FixedArityMatchExpression(
        MatchType type,
        std::array<std::unique_ptr<MatchExpression>, nargs> expressions,
        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(type, std::move(annotation)), _expressions(std::move(expressions)) {}

    const auto& expressions() const {
        return _expressions;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) {
            for (auto& subExpression :
                 static_cast<FixedArityMatchExpression&>(*expression)._expressions) {
                // Since 'subExpression' is a reference to a member of the
                // FixedArityMatchExpression's child array, this assignment replaces the original
                // child with the optimized child.
                subExpression = MatchExpression::optimize(std::move(subExpression));
            }

            return expression;
        };
    }

    std::array<std::unique_ptr<MatchExpression>, nargs> _expressions;
};

}  // namespace mongo
