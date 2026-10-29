// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace change_stream_rewrite {
/**
 * Rewrites a 'userMatch' that will filter events which have been transformed into change stream
 * format so that it can be applied directly to oplog entries. The resulting MatchExpression will
 * preemptively filter out some oplog entries that we know in advance will be filtered out by the
 * 'userMatch' filter, reducing the amount of work that subsequent change streams stage will have to
 * do. The function populates 'backingBsonObjs' with BSONObjs referenced in the newly
 * created MatchExpression. The BSONObjs in the vector have to be kept alive as long as the
 * MatchExpression is alive.
 *
 * The rewrites will only be performed on fields which are present in the 'includeFields' set and
 * absent from the 'excludeFields' set. When 'includeFields' is the empty set, the rewrite defaults
 * to including all paths that can be rewritten.
 */
std::unique_ptr<MatchExpression> rewriteFilterForFields(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* userMatch,
    std::vector<BSONObj>& backingBsonObjs,
    std::set<std::string> includeFields = {},
    std::set<std::string> excludeFields = {});
}  // namespace change_stream_rewrite
}  // namespace mongo
