// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_num_properties.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {

/**
 * MatchExpression for $_internalSchemaMaxProperties keyword. Takes an integer
 * argument that indicates the maximum amount of properties in an object.
 */
class InternalSchemaMaxPropertiesMatchExpression final
    : public InternalSchemaNumPropertiesMatchExpression {
public:
    explicit InternalSchemaMaxPropertiesMatchExpression(
        long long numProperties, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaNumPropertiesMatchExpression(MatchType::INTERNAL_SCHEMA_MAX_PROPERTIES,
                                                     numProperties,
                                                     "$_internalSchemaMaxProperties",
                                                     std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        auto maxProperties = std::make_unique<InternalSchemaMaxPropertiesMatchExpression>(
            numProperties(), _errorAnnotation);
        if (getTag()) {
            maxProperties->setTag(getTag()->clone());
        }
        return maxProperties;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
