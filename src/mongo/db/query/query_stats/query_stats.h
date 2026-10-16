// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/partitioned_cache.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/query_stats_key_hash_index.h"
#include "mongo/db/query/query_stats/rate_limiting.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::query_stats {

extern Counter64& queryStatsStoreSizeEstimateBytesMetric;
extern Counter64& queryStatsStoreSizeMetric;

struct QueryStatsPartitioner {
    // The partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const std::size_t hash, const std::size_t nPartitions) const {
        return hash % nPartitions;
    }
};

/**
 * Computes the budgeted size of a store entry. Note that a QueryStatsEntry only holds the
 * 'recentErrors' LRU cache as a member, so 'sizeof()' does not account for the error entries it
 * heap-allocates as codes are recorded. The worst-case cost of a full cache is reserved up front
 * in 'value.reservedErrorBudgetBytes'.
 */
struct QueryStatsStoreEntryBudgetor {
    size_t operator()(const std::size_t hash, const QueryStatsEntry& value) {
        return sizeof(decltype(value)) + sizeof(decltype(hash)) + value.key->size() +
            value.reservedErrorBudgetBytes +
            // The entry's cached SHA256 'userVisibleHash' is a heap-allocated string that's
            // non-empty only when a secondary index is maintained.
            value.userVisibleHash.size() +
            // When 'internalQueryStatsMatchPushdownOptimizationEnabled' is disabled, the index
            // holds no mappings and shouldn't count towards the memory footprint. When enabled, the
            // 'KeyHashIndex' holds its own heap copy of the userVisibleHash string plus the mapped
            // store key for every indexed entry. The string is counted once per entry, so this
            // slightly over-counts when one keyHash maps to several store keys.
            (internalQueryStatsMatchPushdownOptimizationEnabled.load()
                 ? value.userVisibleHash.size() + sizeof(std::size_t)
                 : 0);
    }
};

/*
 * QueryStatsStore insertion and eviction listener implementation. This class adjusts the
 * 'queryStatsStoreSizeEstimateBytes' and 'numEntries' serverStatus metrics when entries are
 * inserted or evicted, and keeps the 'KeyHashIndex' in sync with the store contents. The listener
 * is bound to the index it maintains, so evicting from one store can never update another store's
 * index. 'index' is null when 'internalQueryStatsMatchPushdownOptimizationEnabled' was disabled.
 */
struct QueryStatsStoreInsertionEvictionListener {
    explicit QueryStatsStoreInsertionEvictionListener(KeyHashIndex* index = nullptr)
        : _index(index) {}

    void onInsert(const std::size_t&, const QueryStatsEntry&, size_t estimatedSize);
    void onEvict(const std::size_t&, const QueryStatsEntry&, size_t estimatedSize);
    void onClear(size_t estimatedSize);

private:
    // TODO SERVER-137022: Once the knob is gone, the index will always be available, so this no
    // longer needs to be a pointer.
    KeyHashIndex* _index;
};

using QueryStatsStore = PartitionedCache<std::size_t,
                                         QueryStatsEntry,
                                         QueryStatsStoreEntryBudgetor,
                                         QueryStatsPartitioner,
                                         QueryStatsStoreInsertionEvictionListener>;

/**
 * One store partition, and the store keys within it that the requested keyHashes resolved to.
 */
struct KeyHashTargetGroup {
    QueryStatsStore::PartitionId partitionId;
    std::vector<std::size_t> storeKeys;
};

/**
 * The outcome of resolving keyHashes: either the resolved target groups (which may be empty, when
 * no requested hash is indexed), or a signal that the keyHash index is not maintained so the caller
 * must scan the store instead.
 */
struct FullStoreScanRequired {};

using KeyLocations = std::variant<std::vector<KeyHashTargetGroup>, FullStoreScanRequired>;

/**
 * A manager for the queryStats store. The usage patterns are as follows:
 *
 * - Updating the queryStats store uses the `getQueryStatsStore()` method. The queryStats store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The queryStats store is resized via `resetSize()`, which sets a new budget and evicts entries
 *   until the store fits within it.
 */
class QueryStatsStoreManager {
public:
    // The query stats store can be configured using these objects on a per-ServiceContext level.
    // This is essentially global, but can be manipulated by unit tests.
    static const ServiceContext::Decoration<std::unique_ptr<QueryStatsStoreManager>> get;
    static const ServiceContext::Decoration<RateLimiter> getRateLimiter;
    static const ServiceContext::Decoration<RateLimiter> getWriteCmdRateLimiter;

    QueryStatsStoreManager(size_t cacheSize, size_t numPartitions)
        : _keyHashIndex(numPartitions),
          _queryStatsStore(std::make_unique<QueryStatsStore>(
              cacheSize,
              numPartitions,
              QueryStatsStoreInsertionEvictionListener{_indexEnabled ? &_keyHashIndex : nullptr})),
          _maxSize(cacheSize) {}

    /**
     * Acquire the instance of the queryStats store.
     */
    QueryStatsStore& getQueryStatsStore() {
        return *_queryStatsStore;
    }

    size_t getMaxSize() {
        return _maxSize.load();
    }

    // TODO SERVER-137022: This function becomes unnecessary once the knob
    // `internalQueryStatsMatchPushdownOptimizationEnabled` is removed.
    bool hasSecondaryIndex() const {
        return _indexEnabled;
    }

    /**
     * Resolves 'keyHashes' to the store keys they are indexed under, grouped by store partition.
     * Returns a 'FullStoreScanRequired' when the keyHash index is not maintained. Otherwise,
     * returns the groups, which may be empty when no requested hash is indexed. Unknown hashes are
     * dropped, and a store key appears once even if several hashes resolve to it.
     */
    KeyLocations findStoreKeysByUserHash(const std::vector<std::string>& keyHashes);

    [[MONGO_MOD_PRIVATE]] size_t numKeyHashes_forTest() const {
        return _keyHashIndex._numKeyHashes_forTest();
    }

    [[MONGO_MOD_PRIVATE]] size_t numMappings_forTest() {
        return _keyHashIndex._numMappings_forTest();
    }

    /**
     * Resize the queryStats store and return the number of evicted
     * entries.
     */
    size_t resetSize(size_t cacheSize) {
        _maxSize.store(cacheSize);
        // The store's eviction listener erases each index mapping as reset() evicts entries.
        return _queryStatsStore->reset(cacheSize);
    }

private:
    // Secondary index over '_queryStatsStore'.
    KeyHashIndex _keyHashIndex;

    const bool _indexEnabled = internalQueryStatsMatchPushdownOptimizationEnabled.load();

    std::unique_ptr<QueryStatsStore> _queryStatsStore;

    /**
     * Max size of the queryStats store. Tracked here to avoid having to recompute after it's
     * divided up into partitions.
     */
    Atomic<size_t> _maxSize;
};

/**
 * Serializes 'key' to its representative query shape.
 */
BSONObj computeRepresentativeShapeKey(OperationContext* opCtx, const Key& key);

/**
 * Computes the user-visible SHA256 hash ('userVisibleHash') of a serialized representative query
 * shape.
 */
std::string computeUserVisibleHash(const BSONObj& representativeShapeKey);

/**
 * Computes the user-visible SHA256 hash ('userVisibleHash') of 'key's representative query shape.
 */
std::string computeUserVisibleHash(OperationContext* opCtx, const Key& key);

/**
 * Acquire a reference to the global queryStats store.
 */
QueryStatsStore& getQueryStatsStore(OperationContext* opCtx);

/**
 * Registers a request for query stats collection. The function may decide not to collect anything,
 * so this should be called for all requests. The decision is made based on query stats rate
 * limiting.
 *
 * The originating command/query does not persist through the end of query execution due to
 * optimizations made to the original query and the expiration of OpCtx across getMores. In order
 * to pair the query stats metrics that are collected at the end of execution with the original
 * query, it is necessary to store the original query during planning and persist it through
 * getMores.
 *
 * During planning, registerRequest is called to serialize the query stats key and save it to
 * OpDebug. If a query's execution is complete within the original operation,
 * collectQueryStatsMongod/collectQueryStatsMongos will call writeQueryStats() and pass along the
 * query stats key to be saved in the query stats store alongside metrics collected.
 *
 * However, OpDebug does not persist through cursor iteration, so if a query's execution will span
 * more than one request/operation, it's necessary to save the query stats context to the cursor
 * upon cursor registration. In these cases, collectQueryStatsMongod/collectQueryStatsMongos will
 * aggregate each operation's metrics within the cursor. Once the request is eventually complete,
 * the cursor calls writeQueryStats() on its destruction.
 *
 * Notes:
 * - It's important to call registerRequest with the original request, before canonicalizing or
 *   optimizing it, in order to preserve the user's input for the query shape.
 * - Calling this affects internal state. It should be called exactly once for each request for
 *   which query stats may be collected.
 * - The std::function argument to construct an abstracted Key is provided to break
 *   library cycles so this library does not need to know how to parse everything. It is done as a
 *   deferred construction callback to ensure that this feature does not impact performance if
 *   collecting stats is not needed due to the feature being disabled or the request being rate
 *   limited.
 */
void registerRequest(OperationContext* opCtx,
                     const NamespaceString& collection,
                     const std::function<std::unique_ptr<Key>(void)>& makeKey);

/**
 * Returns a non-OK status if 'keyBson' is too large or too deeply nested to embed in the
 * $queryStats reply.
 */
Status validateQueryStatsKeyBson(const BSONObj& keyBson);

/**
 * Register a write request. After performing write-relevant checks, it registers a write request
 * using registerRequest().
 */
void registerWriteRequest(OperationContext* opCtx,
                          const NamespaceString& collection,
                          const std::function<std::unique_ptr<Key>(void)>& makeKey);

/**
 * Register the write op at 'writeOpIndex' for a batched write command (e.g., update) on mongos.
 * This differs from registerWriteRequest() above in that it stores the query stats key inside the
 * hash map '_queryStatsInfoForBatchWrites' in OpDebug using 'writeOpIndex' as the key.
 */
void registerWriteRequest(OperationContext* opCtx,
                          const NamespaceString& collection,
                          size_t writeOpIndex,
                          const std::function<std::unique_ptr<Key>(void)>& makeKey);

/**
 * Returns whether or not the current operation should request metrics from remote hosts. This
 * can be either because this query was chosen for query stats collection, the user explicitly
 * requested query stats collection for this query, or this query is an internal query generated
 * by a user query that met one of the first two conditions.
 */
bool shouldRequestRemoteMetrics(const OpDebug& opDebug);
bool shouldRequestRemoteMetrics(const OpDebug& opDebug, size_t opIndex);

/**
 * Convert an optional Duration to a count of Microseconds uint64_t.
 */
inline uint64_t microsecondsToUint64(boost::optional<Microseconds> duration) {
    return static_cast<uint64_t>(duration.value_or(Microseconds{0}).count());
}

/**
 * Convert an optional Duration to a count of Nanoseconds int64_t.
 */
inline int64_t nanosecondsToInt64(boost::optional<Nanoseconds> duration) {
    return duration.has_value() && duration.value() >= Nanoseconds::zero()
        ? static_cast<int64_t>(duration->count())
        : static_cast<int64_t>(-1);
}

/**
 * Snapshot of query stats from CurOp::OpDebug to store in query stats store.
 */
struct QueryStatsSnapshot {
    uint64_t queryExecMicros;
    uint64_t firstResponseExecMicros;
    uint64_t docsReturned;

    uint64_t keysExamined;
    uint64_t docsExamined;
    uint64_t bytesRead;
    int64_t readTimeMicros;
    int64_t workingTimeMillis;
    int64_t cpuNanos;

    uint64_t delinquentAcquisitions;
    int64_t totalAcquisitionDelinquencyMillis;
    int64_t maxAcquisitionDelinquencyMillis;

    uint64_t numInterruptChecks;
    int64_t overdueInterruptApproxMaxMillis;

    bool hasSortStage;
    bool usedDisk;
    bool fromMultiPlanner;
    bool fromPlanCache;
    int64_t planningTimeMicros;
    CardinalityEstimationMethods cardinalityEstimationMethods;
    uint64_t nDocsSampled;
    plan_shape_counters::PlanShapeCounts planShapeCounts;

    uint64_t nMatched;
    uint64_t nUpserted;
    uint64_t nModified;
    uint64_t nDeleted;
    uint64_t nInserted;
    uint64_t nUpdateOps;
    uint64_t nDeleteOps;
    uint64_t keysInserted;
    uint64_t keysDeleted;

    int64_t totalTimeQueuedMicros;
    uint64_t totalAdmissions;
    bool wasLoadShed;
    bool wasDeprioritized;
    bool wasMarkedNonDeprioritizable;

    uint64_t peakTrackedMemBytes;
    uint64_t clusterPeakTrackedMemBytes;

    ErrorCodes::Error errorCode = ErrorCodes::OK;

    bool isErrored() const {
        return errorCode != ErrorCodes::OK;
    }
};

/**
 * Get a snapshot of the metrics to store in query stats from CurOp::OpDebug and others.
 */
QueryStatsSnapshot captureMetrics(const OperationContext* opCtx,
                                  int64_t firstResponseExecutionTime,
                                  const OpDebug::AdditiveMetrics& metrics);

/**
 * Writes query stats to the query stats store for the operation identified by `storeKey`.
 *
 * Direct calls to writeQueryStats in new code should be avoided in favor of calling existing
 * functions:
 *  - collectQueryStatsMongod/collectQueryStatsMongos in the case of requests that span one
 *    operation
 *  - writeQueryStatsOnCursorDisposeOrKill() in the case of requests that span
 *    multiple operations (via getMore)
 */
void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> storeKey,
                     std::unique_ptr<Key> key,
                     const QueryStatsSnapshot& snapshot,
                     std::vector<std::unique_ptr<SupplementalStatsEntry>> supplementalMetrics = {},
                     bool isChangeStreamQuery = false);

/**
 * Called from ClientCursor::dispose/ClusterClientCursorImpl::kill to set up and writeQueryStats()
 * at the end of life of a cursor.
 */
void writeQueryStatsOnCursorDisposeOrKill(
    OperationContext* opCtx,
    boost::optional<size_t> storeKey,
    std::unique_ptr<Key> key,
    bool isChangeStreamQuery,
    boost::optional<Microseconds> firstResponseExecutionTime,
    OpDebug::AdditiveMetrics metrics,
    std::vector<std::unique_ptr<SupplementalStatsEntry>> supplementalMetrics = {});

}  // namespace mongo::query_stats
