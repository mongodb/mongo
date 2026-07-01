/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/db/query/compiler/optimizer/join/join_predicate.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

// The cache key is an opaque string representing the normalized join graph shape.
using JoinPlanCacheKey = std::string;

// Forward-declared so CachedJoinNode can hold a recursive std::unique_ptr<CachedJoinPlan>.
struct CachedJoinPlan;

// Cached single-collection access path. Reuses SolutionCacheData / index-tag machinery
// from the classic plan cache to reconstruct the physical access path on a cache hit.
struct CachedAccessPath {
    const join_ordering::NodeId nodeId;
    std::unique_ptr<const SolutionCacheData> solnCacheData;
};

// Cached extra state for the right-hand side of an INLJ.
struct CachedInljNode {
    const join_ordering::NodeId nodeId;
    const std::string inljForeignIndexName;
};

// Cached binary join node. Left and right children are owned via unique_ptr to close
// the recursion with CachedJoinPlan.
struct CachedJoinNode {
    join_ordering::JoinMethod method;
    std::vector<QSNJoinPredicate> joinPredicates;
    boost::optional<FieldPath> leftEmbeddingField;
    boost::optional<FieldPath> rightEmbeddingField;
    std::unique_ptr<CachedJoinPlan> left;
    std::unique_ptr<CachedJoinPlan> right;
};

// A node in the cached join tree. Wraps the variant so it can be forward-declared.
struct CachedJoinPlan {
    std::variant<CachedAccessPath, CachedJoinNode, CachedInljNode> node;
};

// A full join plan cache entry: a reconstructable plan tree and its invalidation metadata.
struct JoinPlanCacheEntry {
    std::unique_ptr<const CachedJoinPlan> joinTree;
    // TODO SERVER-129266: Add fingerprints
};

/**
 * Global cache for join plans, keyed on a normalized join graph shape string. The cache is
 * registered as a ServiceContext decoration.
 */
class JoinPlanCache {
public:
    /*
     * Returns a shared pointer to the cache entry, or nullptr if not present.
     */
    std::shared_ptr<const JoinPlanCacheEntry> lookup(const JoinPlanCacheKey& key) const;

    /*
     * Inserts or replaces the entry for 'key'. Assumes entry is non-null.
     */
    void put(JoinPlanCacheKey key, std::unique_ptr<JoinPlanCacheEntry> entry);

    /*
     * Removes the entry for 'key' if it exists.
     */
    void remove(const JoinPlanCacheKey& key);

    static JoinPlanCache& get(ServiceContext* svc);

private:
    // Guards concurrent access to _cache. Shared lock for lookups; exclusive lock for mutations.
    mutable RWMutex _mutex;
    // TODO SERVER-129265: replace with LRUKeyValue + proper memory accounting.
    StringMap<std::shared_ptr<JoinPlanCacheEntry>> _cache;
};

}  // namespace mongo
