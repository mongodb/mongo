// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Bogus no-op $where match expression to parse $where in mongos, since mongos doesn't have script
 * engine to compile JS functions.
 *
 * Linked into mongos, instead of the real WhereMatchExpression.
 */
class WhereNoOpMatchExpression final : public WhereMatchExpressionBase {
public:
    explicit WhereNoOpMatchExpression(WhereParams params);

    std::unique_ptr<MatchExpression> clone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    bool runPredicate(const BSONObj& doc) const final;
};

}  // namespace mongo
