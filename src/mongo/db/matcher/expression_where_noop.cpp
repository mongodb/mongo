// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_where_noop.h"

#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {

WhereNoOpMatchExpression::WhereNoOpMatchExpression(WhereParams params)
    : WhereMatchExpressionBase(std::move(params)) {}

bool WhereNoOpMatchExpression::runPredicate(const BSONObj& doc) const {
    MONGO_UNREACHABLE_TASSERT(12712900);
}

std::unique_ptr<MatchExpression> WhereNoOpMatchExpression::clone() const {
    WhereParams params;
    params.code = getCode();
    std::unique_ptr<WhereNoOpMatchExpression> e =
        std::make_unique<WhereNoOpMatchExpression>(std::move(params));
    if (getTag()) {
        e->setTag(getTag()->clone());
    }
    return std::move(e);
}
}  // namespace mongo
