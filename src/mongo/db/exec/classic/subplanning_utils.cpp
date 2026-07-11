// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/exec/classic/subplanning_utils.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"

namespace mongo {

bool SubPlanningUtils::canUseSubplanning(const CanonicalQuery& query) {
    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    const MatchExpression* expr = query.getPrimaryMatchExpression();

    if (!findCommand.getHint().isEmpty())
        return false;
    if (!findCommand.getMin().isEmpty())
        return false;
    if (!findCommand.getMax().isEmpty())
        return false;
    if (findCommand.getTailable())
        return false;
    if (query.getDistinct())
        return false;

    return expr->matchType() == MatchExpression::OR && expr->numChildren() > 0;
}

}  // namespace mongo
