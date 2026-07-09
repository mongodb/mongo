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
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
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

// Live per-Collection version counters, bumped on DDL/sample refresh. Lives as a Collection
// decoration, so it must be default-constructible.
struct CollectionVersionTag {
    // Tracks current state of the collection, so a cached plan built against a different collection
    // state (due to a DDL operation) can be detected as stale. This is bumped whenever a DDL
    // operation runs.
    //
    // Safe as a plain (non-atomic) integer: DDL operations that bump this take the X lock on the
    // Collection and perform a copy-on-write clone inside a WriteUnitOfWork (WUOW); the bump
    // happens on that clone, which is only published (swapped into the catalog) at WUOW commit.
    // Lock-free readers in other threads acquire a snapshot view of the catalog and thus can
    // never observe an in-flight mutation to the Collection instance being cloned, i.e. they see
    // either the fully-old or the fully-new value, never a partial one.
    uint64_t collectionVersion = 0;

    // Tracks the current state of this collection's persisted samples, so a cached plan built
    // from a stale persisted sample can be detected as stale. Bumped whenever the persisted sample
    // is refreshed.
    //
    // TODO SERVER-129270: Handle bumping on sample refresh and explain synchronization mechanism.
    uint64_t sampleVersion = 0;

    bool operator==(const CollectionVersionTag&) const = default;
};


// Captures the state of a collection referenced by a cached join plan: 'uuid' identifies which
// collection the tag belongs to, and 'versionTag' is a copy of its version counters as they were
// when the plan was cached.
struct CollectionTag {
    UUID uuid;
    CollectionVersionTag versionTag;
};

// A full join plan cache entry: a reconstructable plan tree and its invalidation metadata.
struct JoinPlanCacheEntry {
    JoinPlanCacheEntry(std::unique_ptr<CachedJoinPlan> joinTree,
                       join_ordering::NodeId baseNode,
                       std::vector<CollectionTag> collections)
        : joinTree(std::move(joinTree)), baseNode(baseNode), collections(std::move(collections)) {}

    // Reconstructable plan.
    std::unique_ptr<const CachedJoinPlan> joinTree;
    const join_ordering::NodeId baseNode;

    // One CollectionTag per collection referenced by 'joinTree', captured when this entry was
    // cached. Each tag's 'uuid' is matched against the live collections acquired by the query
    // to find the corresponding entry. The version tags of the entry are compared to detect
    // invalidation on plan cache lookup.
    std::vector<CollectionTag> collections;

    // TODO: (SERVER-130368) Add relevant index invalidation.
};

/*
 * Snapshots the current CollectionVersionTag for every collection in 'mca'.
 */
std::vector<CollectionTag> makeCollectionTags(const MultipleCollectionAccessor& mca);

/*
 * Returns true iff every tag in 'tags' still matches the corresponding live collection in 'mca'.
 * A referenced collection that's no longer resolvable in 'mca' (dropped/renamed) counts as a
 * mismatch, not an error.
 */
bool areCollectionTagsCurrent(const std::vector<CollectionTag>& tags,
                              const MultipleCollectionAccessor& mca);

/**
 * Global cache for join plans, keyed on a normalized join graph shape string. The cache is
 * registered as a ServiceContext decoration.
 */
class JoinPlanCache {
public:
    /*
     * Current tags for the collection state, updated on DDLs and sample refresh. See
     * CollectionVersionTag's field comments for per-field synchronization guarantees.
     */
    inline static const auto currentVersionTags =
        Collection::declareDecoration<CollectionVersionTag>();

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
