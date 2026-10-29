// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Matches arrays whose elements are all unique. When comparing elements,
 *
 *  - strings are always compared using the "simple" string comparator; and
 *  - objects are compared in a field order-independent manner.
 */
class InternalSchemaUniqueItemsMatchExpression final : public ArrayMatchingMatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaUniqueItems"sv;

    explicit InternalSchemaUniqueItemsMatchExpression(
        boost::optional<std::string_view> path, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ArrayMatchingMatchExpression(
              MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS, path, std::move(annotation)) {}

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400219);
    }

    void resetChild(size_t i, MatchExpression*) final {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    void debugString(StringBuilder& builder, int indentationLevel) const final;

    bool equivalent(const MatchExpression* other) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    std::unique_ptr<MatchExpression> clone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const auto& getComparator() const {
        return _comparator;
    }

private:
    // The comparator to use when comparing BSONElements, which will never use a collation.
    UnorderedFieldsBSONElementComparator _comparator;
};
}  // namespace mongo
