// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/db/query/compiler/optimizer/join/join_predicate.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/partitioned_cache.h"
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
#include <mutex>
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

    // Heap-allocated bytes owned by this node. The node's own 'sizeof' is accounted for by the
    // enclosing CachedJoinPlan, so it is deliberately excluded here to avoid double-counting the
    // inline variant storage.
    size_t estimateHeapBytes() const;
};

// Cached extra state for the right-hand side of an INLJ.
struct CachedInljNode {
    const join_ordering::NodeId nodeId;
    const std::string inljForeignIndexName;

    size_t estimateHeapBytes() const;
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

    size_t estimateHeapBytes() const;
};

// A node in the cached join tree. Wraps the variant so it can be forward-declared.
struct CachedJoinPlan {
    std::variant<CachedAccessPath, CachedJoinNode, CachedInljNode> node;

    // Total memory footprint of this subtree, including this node's own sizeof plus the
    // heap-allocated bytes of the active variant alternative (and, recursively, its children).
    size_t estimateObjectSizeInBytes() const;

    BSONObj toBSON() const;
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

// Hash of the set of indexes relevant to a single node of the join graph, including each index's
// definition. Computed by 'join_ordering::makeNodeFingerprints' (index_fingerprint.h) from the
// live catalog, both when an entry is stored and when a stored entry needs revalidating.
using NodeFingerprint = std::size_t;

// A full join plan cache entry: a reconstructable plan tree and its invalidation metadata.
struct JoinPlanCacheEntry {
    JoinPlanCacheEntry(std::unique_ptr<CachedJoinPlan> joinTree,
                       join_ordering::NodeId baseNode,
                       std::vector<CollectionTag> collections,
                       std::vector<NodeFingerprint> nodeFingerprints);

    // Estimated memory footprint of this entry. Declared first so it can be computed from the
    // constructor's arguments before they are moved into the members below.
    const size_t estimatedEntrySizeBytes;

    // Reconstructable plan.
    std::unique_ptr<const CachedJoinPlan> joinTree;
    const join_ordering::NodeId baseNode;

    // One fingerprint per join graph node, indexed by NodeId in ascending order - exactly the
    // layout 'join_ordering::makeNodeFingerprints' returns. Captured when this entry was cached
    // and compared against freshly computed fingerprints when the collection tags no longer
    // validate, so that an index DDL irrelevant to this plan does not force a replan.
    std::vector<NodeFingerprint> nodeFingerprints;

    /*
     * Returns a copy of this entry's collection tags. A copy rather than a reference because the
     * tags can be refreshed concurrently by 'refreshCollectionTags'.
     */
    std::vector<CollectionTag> getCollectionTags() const;

    /*
     * Adopts the version counters in 'tags' for the collections this entry already references.
     * Called when a lookup found the collection versions bumped but the relevant-index fingerprints
     * unchanged: taking on the newer versions lets subsequent lookups take the fast path instead of
     * re-fingerprinting on every query.
     *
     * 'tags' must have been snapshotted from the very same catalog state the fingerprints were
     * just validated against, so that this entry never claims to be current for a collection state
     * it was not checked against. It must also describe the same set of collections, which the join
     * graph shape the entry is keyed on guarantees; the order is free to differ, as tags are paired
     * up by UUID.
     */
    void refreshCollectionTags(const std::vector<CollectionTag>& tags);

private:
    // Guards '_collections', held only for the copy out of, or the counter updates to, that vector.
    // Mutable so that reading the tags out of a const entry (e.g. when serializing the cache for
    // $joinPlanCacheStats) is possible.
    mutable std::mutex _collectionsMutex;

    // One CollectionTag per collection referenced by 'joinTree', compared against the live
    // collections to detect invalidation on plan cache lookup.
    std::vector<CollectionTag> _collections;
};

/*
 * Snapshots the current CollectionVersionTag for every collection in 'mca'.
 */
std::vector<CollectionTag> makeCollectionTags(const MultipleCollectionAccessor& mca);

/*
 * Renders 'tags' for logging as a BSONObj that maps from collection UUID to collection version.
 * Keyed by UUID rather than emitted as a list because two sets of tags being compared are not in a
 * guaranteed order.
 */
BSONObj collectionVersionsForLog(const std::vector<CollectionTag>& tags);

/*
 * The action to take with a cached entry, given its collection tags.
 */
enum class CollectionTagStatus {
    // Reuse the entry.
    kCurrent,

    // A DDL ran, but it may have been on an index the plan could never use. Compare the
    // relevant-index fingerprints to decide.
    kNeedsIndexRevalidation,

    // Discard the entry; no fingerprint can rescue it.
    kStale,
};

/*
 * Classifies 'tags' against the live collections in 'mca'. When several collections disagree, the
 * most restrictive status wins, in the order kStale > kNeedsIndexRevalidation > kCurrent.
 */
CollectionTagStatus classifyCollectionTags(const std::vector<CollectionTag>& tags,
                                           const MultipleCollectionAccessor& mca);

// Functor for estimating the memory footprint of a join plan cache entry.
struct JoinPlanCacheBudgetEstimator {
    size_t operator()(const JoinPlanCacheKey& key,
                      const std::shared_ptr<JoinPlanCacheEntry>& entry) const {
        return entry->estimatedEntrySizeBytes + key.size();
    }
};

// Maps a key to a partition id. The default Partitioner has no overload for std::string, so a
// custom functor hashing the key is required.
struct JoinPlanCacheKeyPartitioner {
    std::size_t operator()(const JoinPlanCacheKey& key, std::size_t nPartitions) const {
        return std::hash<JoinPlanCacheKey>{}(key) % nPartitions;
    }
};

// The backing store: a budget-based, LRU-evicting PartitionedCache. The mapped type is a
// shared_ptr so lookups can hand out a stable reference-counted handle on the entry, and the cache
// copies the pointer rather than the entry. KeyHasher/Eq default to those for std::string.
using JoinPlanCacheStore = PartitionedCache<JoinPlanCacheKey,
                                            std::shared_ptr<JoinPlanCacheEntry>,
                                            JoinPlanCacheBudgetEstimator,
                                            JoinPlanCacheKeyPartitioner,
                                            NoopInsertionEvictionListener>;

namespace join_ordering {
/*
 * Bumps the live 'collectionVersion' on 'writableColl' so that any join plan cached against the
 * collection's pre-DDL state is detected as stale on the next plan cache lookup. Must be called
 * from a strict DDL operation that holds the X lock on the collection and operates on the
 * writable (copy-on-write) Collection clone inside the mutating WriteUnitOfWork; the bump is only
 * published when that WUOW commits. See CollectionVersionTag's field comments for the
 * synchronization guarantees that make the non-atomic increment safe.
 */
[[MONGO_MOD_PUBLIC]] void bumpCollectionVersionForDDL(Collection* writableColl);
}  // namespace join_ordering

/**
 * Global cache for join plans, keyed on a normalized join graph shape string. The cache is
 * registered as a ServiceContext decoration. The underlying PartitionedCache is internally
 * synchronized per-partition, so no external locking is required.
 */
class JoinPlanCache {
public:
    explicit JoinPlanCache(size_t cacheSizeBytes, size_t numPartitions)
        : _cache(cacheSizeBytes, numPartitions) {}

    /*
     * Current tags for the collection state, updated on DDLs and sample refresh. See
     * CollectionVersionTag's field comments for per-field synchronization guarantees.
     */
    inline static const auto currentVersionTags =
        Collection::declareDecoration<CollectionVersionTag>();

    /*
     * Returns a shared pointer to the cache entry, or nullptr if not present. The entry is mutable:
     * a lookup which revalidates it against the current catalog updates its collection tags in
     * place. Entries are internally synchronized, as several operations can share one.
     */
    std::shared_ptr<JoinPlanCacheEntry> lookup(const JoinPlanCacheKey& key) const;

    /*
     * Inserts or replaces the entry for 'key'. Assumes entry is non-null. Returns the number of
     * older entries evicted to fit this one within the cache's memory budget.
     */
    size_t put(JoinPlanCacheKey key, std::unique_ptr<JoinPlanCacheEntry> entry);

    /*
     * Removes the entry for 'key' if it exists.
     */
    void remove(const JoinPlanCacheKey& key);

    /*
     * Resets the total memory budget, evicting least-recently-used entries as needed to fit.
     * Returns the number of entries evicted.
     */
    size_t reset(size_t cacheSizeBytes);

    /*
     * Returns the current total memory footprint (in bytes) of all cached entries.
     */
    size_t size() const;

    /*
     * Serializes every entry in the cache to BSON, one object per entry, for the
     * $joinPlanCacheStats aggregation stage. Consistency across partitions is not guaranteed.
     */
    std::vector<BSONObj> serializeEntries() const;

    static JoinPlanCache& get(ServiceContext* svc);

private:
    JoinPlanCacheStore _cache;
};

/*
 * Serializes a single join plan cache entry (and its key) to BSON for $joinPlanCacheStats. Exposed
 * as a free function so it can be unit tested independently of the cache.
 */
BSONObj joinPlanCacheEntryToBSON(const JoinPlanCacheKey& key, const JoinPlanCacheEntry& entry);

}  // namespace mongo
