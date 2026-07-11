// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * A match expression similar to $elemMatch, but only matches arrays for which every element
 * matches the sub-expression.
 */
class InternalSchemaAllElemMatchFromIndexMatchExpression final
    : public ArrayMatchingMatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaAllElemMatchFromIndex"sv;
    static constexpr int kNumChildren = 1;

    InternalSchemaAllElemMatchFromIndexMatchExpression(
        boost::optional<std::string_view> path,
        long long index,
        std::unique_ptr<ExpressionWithPlaceholder> expression,
        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    /**
     * Returns an index of the first element of the array this match expression applies to.
     */
    long long startIndex() const {
        return _index;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return kNumChildren;
    }

    MatchExpression* getChild(size_t i) const final {
        tassert(6400200, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        return _expression->getFilter();
    }

    void resetChild(size_t i, MatchExpression* other) override {
        tassert(6329407, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        _expression->resetFilter(other);
    };


    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const ExpressionWithPlaceholder* getExpression() const {
        return _expression.get();
    }

    ExpressionWithPlaceholder* getExpression() {
        return _expression.get();
    }

private:
    long long _index;
    std::unique_ptr<ExpressionWithPlaceholder> _expression;
};
}  // namespace mongo
