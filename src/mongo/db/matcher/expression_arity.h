// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

#include <boost/optional.hpp>

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

    ~FixedArityMatchExpression() override = default;

    void debugString(StringBuilder& debug, int indentationLevel) const final {
        _debugAddSpace(debug, indentationLevel);

        BSONObjBuilder builder;
        serialize(&builder, {});
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

    MatchExpression* releaseChild(size_t i) {
        tassert(10806402, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        return _expressions[i].release();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329406, "Out-of-bounds access to child of MatchExpression.", i < numChildren());
        _expressions[i].reset(other);
    }

    /**
     * The name of this MatchExpression.
     */
    virtual std::string_view name() const = 0;

    /**
     * Serializes each subexpression sequentially in a BSONArray.
     */
    void serialize(BSONObjBuilder* builder,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final {
        BSONArrayBuilder exprArray(builder->subarrayStart(name()));
        for (const auto& expr : _expressions) {
            BSONObjBuilder exprBuilder(exprArray.subobjStart());
            expr->serialize(&exprBuilder, opts, includePath);
            exprBuilder.doneFast();
        }
        exprArray.doneFast();
    }

    /**
     * Clones this MatchExpression by recursively cloning each sub-expression.
     */
    std::unique_ptr<MatchExpression> clone() const final {
        std::array<std::unique_ptr<MatchExpression>, nargs> clonedExpressions;
        std::transform(_expressions.begin(),
                       _expressions.end(),
                       clonedExpressions.begin(),
                       [](const auto& orig) {
                           return orig ? orig->clone() : std::unique_ptr<MatchExpression>(nullptr);
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
    std::array<std::unique_ptr<MatchExpression>, nargs> _expressions;
};

}  // namespace mongo
