// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/matcher/matcher.h"

namespace mongo {

namespace exec::matcher {

void MatchesSingleElementEvaluator::visit(const TextMatchExpression* expr) {
    // Text match expressions force the selection of the text index and always generate EXACT
    // index bounds (which causes the MatchExpression node to be trimmed), so we don't currently
    // implement any explicit text matching logic here. SERVER-17648 tracks the work to
    // implement a real text matcher.
    //
    // TODO: simply returning 'true' here isn't quite correct. First, we should be overriding
    // matches() instead of matchesSingleElement(), because the latter is only ever called if
    // the matched document has an element with path "_fts". Second, there are scenarios where
    // we will use the untrimmed expression tree for matching (for example, when deciding
    // whether or not to retry an operation that throws WriteConflictException); in those cases,
    // we won't be able to correctly determine whether or not the object matches the expression.
    _result = true;
}

void MatchesSingleElementEvaluator::visit(const TextNoOpMatchExpression* expr) {
    MONGO_UNREACHABLE_TASSERT(9713601);
}

}  // namespace exec::matcher
}  // namespace mongo
