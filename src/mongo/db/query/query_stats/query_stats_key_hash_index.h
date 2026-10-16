// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/partitioned.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <boost/optional/optional.hpp>

namespace mongo::query_stats {

/**
 * Partitioning function for 'KeyHashIndex' that relies on the user-visible hash.
 */
struct KeyHashPartitioner {
    std::size_t operator()(const std::string& userVisibleHash,
                           const std::size_t nPartitions) const {
        return absl::HashOf(userVisibleHash) % nPartitions;
    }
};

/**
 * A secondary index over the query stats store, mapping the user-visible hash that $queryStats
 * emits to the set of absl::HashOf(Key) store keys indexed under it. A user-visible hash can map to
 * several store keys, so a $match on keyHash can read just those entries instead of scanning the
 * store.
 *
 * The store's insertion/eviction listener keeps it in sync, and mappings are added only when
 * 'internalQueryStatsMatchPushdownOptimizationEnabled' was enabled at startup; otherwise the index
 * stays empty and callers scan the entire store. It is partitioned by user-visible hash, and
 * callers take an index partition lock only while already holding the relevant store partition lock
 * (never the reverse), one index partition at a time.
 */
class KeyHashIndex {
public:
    explicit KeyHashIndex(size_t numPartitions)
        : _userVisibleHashToStoreKey(numPartitions), _numPartitions(numPartitions) {}

private:
    friend class QueryStatsStoreManager;
    friend struct QueryStatsStoreInsertionEvictionListener;
    friend class KeyHashIndexTest;

    using StoreKeySet = absl::flat_hash_set<std::size_t>;

    /**
     * The number of distinct user-visible hashes indexed. Not the number of indexed store keys,
     * which can be larger when one user-visible hash maps to several store keys.
     */
    [[MONGO_MOD_PRIVATE]] size_t _numKeyHashes_forTest() const {
        return _userVisibleHashToStoreKey.size();
    }

    /**
     * The total number of index mappings: the sum of the per-user-visible-hash store key sets,
     * which equals the number of indexed store entries.
     */
    [[MONGO_MOD_PRIVATE]] size_t _numMappings_forTest() {
        size_t numMappings = 0;
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto partition = _userVisibleHashToStoreKey.lockOnePartitionById(partitionId);
            for (const auto& entry : *partition) {
                numMappings += entry.second.size();
            }
        }
        return numMappings;
    }

    /**
     * Adds 'storeKey' to the set indexed under 'userVisibleHash'.
     */
    void _insert(const std::string& userVisibleHash, std::size_t storeKey) {
        auto partition = _userVisibleHashToStoreKey.lockOnePartition(userVisibleHash);
        partition->try_emplace(userVisibleHash).first->second.insert(storeKey);
    }

    /**
     * Returns the store keys indexed under 'userVisibleHash', or boost::none when the index holds
     * no mapping for it. Absence could also be due to
     * `internalQueryStatsMatchPushdownOptimizationEnabled` being disabled. Returned by value so the
     * caller does not hold the index partition lock.
     */
    boost::optional<StoreKeySet> _lookup(const std::string& userVisibleHash) {
        auto partition = _userVisibleHashToStoreKey.lockOnePartition(userVisibleHash);
        if (auto it = partition->find(userVisibleHash); it != partition->end()) {
            return it->second;
        }
        return boost::none;
    }

    /**
     * Removes 'storeKey' from the set indexed under 'userVisibleHash', dropping the set once it is
     * empty.
     */
    void _erase(const std::string& userVisibleHash, std::size_t storeKey) {
        auto partition = _userVisibleHashToStoreKey.lockOnePartition(userVisibleHash);
        if (auto it = partition->find(userVisibleHash); it != partition->end()) {
            it->second.erase(storeKey);
            if (it->second.empty()) {
                partition->erase(it);
            }
        }
    }

    // 'userVisibleHash' (SHA256, base64) -> set of 'absl::HashOf(Key)' store keys.
    Partitioned<absl::flat_hash_map<std::string, StoreKeySet>, KeyHashPartitioner>
        _userVisibleHashToStoreKey;

    const size_t _numPartitions;
};

}  // namespace mongo::query_stats
