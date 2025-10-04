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
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class InternalSchemaMaxLengthMatchExpression final : public InternalSchemaStrLengthMatchExpression {

public:
    InternalSchemaMaxLengthMatchExpression(boost::optional<StringData> path,
                                           long long strLen,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : InternalSchemaStrLengthMatchExpression(MatchType::INTERNAL_SCHEMA_MAX_LENGTH,
                                                 path,
                                                 strLen,
                                                 "$_internalSchemaMaxLength"_sd,
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
