// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_num_array_items.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * MatchExpression for $_internalSchemaMinItems keyword. Takes an integer argument that indicates
 * the minimum amount of elements in an array.
 */
class InternalSchemaMinItemsMatchExpression final
    : public InternalSchemaNumArrayItemsMatchExpression {
public:
    InternalSchemaMinItemsMatchExpression(boost::optional<std::string_view> path,
                                          long long numItems,
                                          clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaNumArrayItemsMatchExpression(INTERNAL_SCHEMA_MIN_ITEMS,
                                                     path,
                                                     numItems,
                                                     "$_internalSchemaMinItems"sv,
                                                     std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<InternalSchemaMinItemsMatchExpression> minItems =
            std::make_unique<InternalSchemaMinItemsMatchExpression>(
                path(), numItems(), _errorAnnotation);
        if (getTag()) {
            minItems->setTag(getTag()->clone());
        }
        return minItems;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
