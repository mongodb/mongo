/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_enumerator/enumerator_memo.h"
#include "mongo/db/query/projection.h"

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
