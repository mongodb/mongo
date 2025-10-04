/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
