/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <algorithm>
#include <array>

#include "mongo/db/matcher/expression.h"
#include "mongo/stdx/memory.h"

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

    void debugString(StringBuilder& debug, int level) const final {
        _debugAddSpace(debug, level);

        BSONObjBuilder builder;
        serialize(&builder);
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

    /**
     * Takes ownership of the MatchExpressions in 'expressions'.
     */
    void init(std::array<std::unique_ptr<MatchExpression>, nargs> expressions) {
        _expressions = std::move(expressions);
    }

    /**
     * The name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    /**
     * Serializes each subexpression sequentially in a BSONArray.
     */
    void serialize(BSONObjBuilder* builder) const final {
        BSONArrayBuilder exprArray(builder->subarrayStart(name()));
        for (const auto& expr : _expressions) {
            BSONObjBuilder exprBuilder(exprArray.subobjStart());
            expr->serialize(&exprBuilder);
            exprBuilder.doneFast();
        }
        exprArray.doneFast();
    }

    /**
     * Clones this MatchExpression by recursively cloning each sub-expression.
     */
    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::unique_ptr<T> clone = stdx::make_unique<T>();
        std::array<std::unique_ptr<MatchExpression>, nargs> clonedExpressions;
        std::transform(_expressions.begin(),
                       _expressions.end(),
                       clonedExpressions.begin(),
                       [](const auto& orig) {
                           return orig ? orig->shallowClone()
                                       : std::unique_ptr<MatchExpression>(nullptr);
                       });
        clone->_expressions = std::move(clonedExpressions);

        if (getTag()) {
            clone->setTag(getTag()->clone());
        }

        return std::move(clone);
    }

protected:
    explicit FixedArityMatchExpression(MatchType type) : MatchExpression(type) {}

    const auto& expressions() const {
        return _expressions;
    }

private:
    std::array<std::unique_ptr<MatchExpression>, nargs> _expressions;
};

}  // namespace mongo
