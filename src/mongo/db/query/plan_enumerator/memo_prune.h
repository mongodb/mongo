// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/plan_enumerator/enumerator_memo.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace plan_enumerator {

/*
 * Additional information about the query that is required for pruning the memo.
 */
struct QueryPruningInfo {
    const projection_ast::Projection* projection;
    const boost::optional<stdx::unordered_set<std::string>>& sortPatFields;
    const BSONObj& shardKey;
    const std::vector<IndexEntry>* indices;
};

/*
 * Prunes index assignments from the memo if they are interchangeable with another existing
 * assignment. Interchangeable means they provide no additional value or information over the other
 * index. For example {a: 1} vs {a: 1, b: 1} if we have a query a=1 are interchangeable.
 * This pruning is done as a step after memo preparation to make sure we output a set of plans that
 * are sufficiently distinct from each other.
 * Returns true if any indexes were pruned, otherwise false.
 */
bool pruneMemoOfDupIndexes(stdx::unordered_map<MemoID, std::unique_ptr<NodeAssignment>>& memo,
                           const QueryPruningInfo& queryInfo);

}  // namespace plan_enumerator
}  // namespace mongo
