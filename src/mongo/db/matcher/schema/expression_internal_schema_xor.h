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

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaXor keyword. Returns true only if exactly
 * one of its child nodes matches.
 */
class InternalSchemaXorMatchExpression final : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaXor"_sd;

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

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> clone() const {
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

    void serialize(BSONObjBuilder* out, const SerializationOptions& opts) const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
