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

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_num_array_items.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * MatchExpression for $_internalSchemaMaxItems keyword. Takes an integer argument that indicates
 * the maximum amount of elements in an array.
 */
class InternalSchemaMaxItemsMatchExpression final
    : public InternalSchemaNumArrayItemsMatchExpression {
public:
    InternalSchemaMaxItemsMatchExpression(boost::optional<StringData> path,
                                          long long numItems,
                                          clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaNumArrayItemsMatchExpression(INTERNAL_SCHEMA_MAX_ITEMS,
                                                     path,
                                                     numItems,
                                                     "$_internalSchemaMaxItems"_sd,
                                                     std::move(annotation)) {}

    std::unique_ptr<MatchExpression> clone() const final {
        std::unique_ptr<InternalSchemaMaxItemsMatchExpression> maxItems =
            std::make_unique<InternalSchemaMaxItemsMatchExpression>(
                path(), numItems(), _errorAnnotation);
        if (getTag()) {
            maxItems->setTag(getTag()->clone());
        }
        return maxItems;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};
}  // namespace mongo
