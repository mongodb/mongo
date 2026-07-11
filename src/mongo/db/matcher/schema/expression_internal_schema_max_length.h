// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class InternalSchemaMaxLengthMatchExpression final : public InternalSchemaStrLengthMatchExpression {

public:
    InternalSchemaMaxLengthMatchExpression(boost::optional<std::string_view> path,
                                           long long strLen,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaStrLengthMatchExpression(MatchType::INTERNAL_SCHEMA_MAX_LENGTH,
                                                 path,
                                                 strLen,
                                                 "$_internalSchemaMaxLength"sv,
                                                 std::move(annotation)) {}

    Validator getComparator() const final {
        return [strLen = strLen()](int lenWithoutNullTerm) {
            return lenWithoutNullTerm <= strLen;
        };
    }

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<InternalSchemaMaxLengthMatchExpression> maxLen =
            std::make_unique<InternalSchemaMaxLengthMatchExpression>(
                path(), strLen(), _errorAnnotation);
        if (getTag()) {
            maxLen->setTag(getTag()->clone());
        }
        return maxLen;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
