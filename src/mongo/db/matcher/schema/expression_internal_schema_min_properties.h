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
 * MatchExpression for $_internalSchemaMinProperties keyword. Takes an integer
 * argument that indicates the minimum amount of properties in an object.
 */
class InternalSchemaMinPropertiesMatchExpression final
    : public InternalSchemaNumPropertiesMatchExpression {
public:
    explicit InternalSchemaMinPropertiesMatchExpression(
        long long numProperties, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaNumPropertiesMatchExpression(MatchType::INTERNAL_SCHEMA_MIN_PROPERTIES,
                                                     numProperties,
                                                     "$_internalSchemaMinProperties",
                                                     std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        auto minProperties = std::make_unique<InternalSchemaMinPropertiesMatchExpression>(
            numProperties(), _errorAnnotation);
        if (getTag()) {
            minProperties->setTag(getTag()->clone());
        }
        return minProperties;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
