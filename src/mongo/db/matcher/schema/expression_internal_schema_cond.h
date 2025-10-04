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
#include "mongo/db/matcher/expression_arity.h"
#include "mongo/db/matcher/expression_visitor.h"

#include <array>
#include <memory>
#include <utility>

namespace mongo {

/**
 * A MatchExpression that represents the ternary "conditional" operator.
 */
class InternalSchemaCondMatchExpression final
    : public FixedArityMatchExpression<InternalSchemaCondMatchExpression, 3> {
public:
    static constexpr StringData kName = "$_internalSchemaCond"_sd;

    explicit InternalSchemaCondMatchExpression(
        std::array<std::unique_ptr<MatchExpression>, 3> expressions,
        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : FixedArityMatchExpression(
              MatchType::INTERNAL_SCHEMA_COND, std::move(expressions), std::move(annotation)) {}

    const MatchExpression* condition() const {
        return expressions()[0].get();
    }

    const MatchExpression* thenBranch() const {
        return expressions()[1].get();
    }

    const MatchExpression* elseBranch() const {
        return expressions()[2].get();
    }

    StringData name() const final {
        return kName;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
