// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * MatchExpression for $_internalSchemaXor keyword. Returns true only if exactly
 * one of its child nodes matches.
 */
class InternalSchemaXorMatchExpression final : public ListOfMatchExpression {
public:
    static constexpr std::string_view kName = "$_internalSchemaXor"sv;

    InternalSchemaXorMatchExpression(clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(INTERNAL_SCHEMA_XOR, std::move(annotation), {}) {}
    InternalSchemaXorMatchExpression(std::vector<std::unique_ptr<MatchExpression>> expressions,
                                     clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(
              INTERNAL_SCHEMA_XOR, std::move(annotation), std::move(expressions)) {}
    InternalSchemaXorMatchExpression(std::unique_ptr<MatchExpression> expression,
                                     clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ListOfMatchExpression(
              INTERNAL_SCHEMA_XOR, std::move(annotation), makeVector(std::move(expression))) {}

    std::unique_ptr<MatchExpression> clone() const override {
        auto xorCopy = std::make_unique<InternalSchemaXorMatchExpression>(_errorAnnotation);
        xorCopy->reserve(numChildren());
        for (size_t i = 0; i < numChildren(); ++i) {
            xorCopy->add(getChild(i)->clone());
        }
        if (getTag()) {
            xorCopy->setTag(getTag()->clone());
        }
        return xorCopy;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
