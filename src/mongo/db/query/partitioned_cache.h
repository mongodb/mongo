// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/partitioned.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo {

/**
 * A partitioned cache combines a size-bounded map (LRU-based entry eviction) with a partition
 * function which allows reducing contention.
 */
template <class KeyType,
          class ValueType,
          class KeyBudgetEstimator,
          class Partitioner,
          class InsertionEvictionListener,
          class KeyHasher = std::hash<KeyType>,
          class Eq = std::equal_to<KeyType>>
class PartitionedCache {
private:
    PartitionedCache(const PartitionedCache&) = delete;
    PartitionedCache& operator=(const PartitionedCache&) = delete;

public:
    using Lru = LRUKeyValue<KeyType,
                            ValueType,
                            KeyBudgetEstimator,
                            InsertionEvictionListener,
                            KeyHasher,
                            Eq>;
    using Partition = typename Partitioned<Lru, Partitioner>::OnePartition;
    using PartitionId = typename Partitioned<Lru, Partitioner>::PartitionId;

    /**
     * Initialize plan cache with the total cache size in bytes and number of partitions.
     *
     * Important edge cases to consider include:
     *
     * 1. Adding an entry that is larger than the max partition size to a non-empty partition.
     *
     *     This will evict both entries. This is because entries are evicted from the partition in
     * order of least recently used. Thus, the oldest, small entry will be evicted first but the
     * partition will still be over budget with the new, too-large entry so it will be evicted as
     * well.
     *
     *   2. Adding a queryStats store entry that is smaller than the overall cache size but larger
     * than single partition max size.
     *
     *       It is not possible to write entries to the cache that are larger than a single
     * partition's max size, even if it is smaller than the entire cache max size. This is because
     * the cache's budget is configured/regulated on the partition level (cacheSize /
     * numPartitions). This makes sense as each entry is written to a specific partition, but might
     * not be immediately obvious so worthy to highlight.
     *
     *   3. Too few partitions can cause unnecessary evictions
     *
     *      Every class that implements the PartitionedCache template provides a partitioner() that
     * returns the id of the partition to which to write the entry. In existing implementations,
     * partitioner() returns the remainder after dividing the entry's key hash by numPartitions. In
     * the case where we have only two partitions, every odd key hash will be written to the first
     * partition (and vice versa). In this way, it can quickly be the case that one partition
     * fills up completely but the partitioner() call keeps returning the already full partition and
     * the cache evict old entries from it to put the new one in. At the end of all the write
     * operations, the cache is below it's budget (as the second partition is only partially full)
     * but we don't have all the entries we expect.  It is therefore important to have sufficient
     * enough number of partitions so the entries can be more equally dispersed to avoid unnecessary
     * evictions.
     */
    explicit PartitionedCache(size_t cacheSize, size_t numPartitions)
        : _numPartitions(numPartitions) {
        invariant(numPartitions > 0);
        Lru lru{cacheSize / numPartitions};
        _partitionedCache =
            std::make_unique<Partitioned<Lru, Partitioner>>(numPartitions, std::move(lru));
    }

    ~PartitionedCache() = default;
    /**
     * Inserts the provided <key, value> into the partition associated with that key. Returns the
     * number of older entries evicted to fit this new one.
     */
    size_t put(const KeyType& key, ValueType value) {
        auto partition = _partitionedCache->lockOnePartition(key);
        return partition->add(key, std::move(value));
    }
    /**
     * Inserts the provided <key, value> into the specified partition. Returns the number of older
     * entries evicted to fit this new one.
     */
    size_t put(const KeyType& key, ValueType value, Partition& partition) {
        return partition->add(key, std::move(value));
    }

    StatusWith<ValueType*> lookup(const KeyType& key) const {
        auto partition = _partitionedCache->lockOnePartition(key);
        auto entry = partition->get(key);
        if (!entry.isOK()) {
            return {entry.getStatus()};
        }

        return {&entry.getValue()->second};
    }

    /**
     * Lookup an entry and also return a lock over the partition. The lock is returned whether
     * or not the entry is found.
     */
    std::pair<StatusWith<ValueType*>, Partition> getWithPartitionLock(const KeyType& key) const {
        auto partition = _partitionedCache->lockOnePartition(key);
        auto entry = partition->get(key);
        if (!entry.isOK()) {
            return std::make_pair(entry.getStatus(), std::move(partition));
        }

        return std::make_pair(StatusWith{&entry.getValue()->second}, std::move(partition));
    }

    /**
     * Scans all partitions, locking one partition at a time, and returns copies of up to
     * 'maxCandidates' entries whose extracted sort keys are the best candidates according to
     * 'isBetter', in no particular order.
     * - 'extract' maps an entry to a cheap, copyable sort key and is called while the partition
     * lock is held, so it must be inexpensive.
     * - 'isBetter(a, b)' must be a strict weak ordering returning true iff the sort key 'a' is a
     * better candidate than 'b' (e.g. 'a > b' for a descending top-K).
     * - 'interruptCheck' is called between each partition lock once.
     *
     * Only one partition is locked at a time. This means entries selected from different partitions
     * are not guaranteed to have coexisted at a single point in time, and thus we do not return
     * top-K entries from a single point-in-time. This would require locking the entire store, which
     * would affect performance of concurrent queries.
     */
    template <typename Extract, typename Better, typename InterruptCheck>
    std::vector<std::pair<KeyType, ValueType>> topKCandidateEntries(
        size_t maxCandidates,
        Extract&& extract,
        Better&& isBetter,
        InterruptCheck&& interruptCheck) const {
        if (maxCandidates == 0) {
            return {};
        }

        using SortKey = std::decay_t<std::invoke_result_t<Extract, const ValueType&>>;
        struct HeapEntry {
            SortKey sortKey;
            KeyType key;
            std::unique_ptr<ValueType> value;
        };
        // Binary heap where the front is always the worst surviving candidate, so it can be
        // replaced as soon as a better one arrives.
        auto heapComp = [&isBetter](const HeapEntry& a, const HeapEntry& b) {
            return isBetter(a.sortKey, b.sortKey);
        };
        std::vector<HeapEntry> heap;

        // Adds a candidate to the heap, keeping it bounded to the best 'maxCandidates' seen so far.
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            interruptCheck();
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);
            for (auto&& [key, entry] : *lockedPartition) {
                auto sortKey = extract(entry);
                // Only copy entries that provisionally make the cut.
                if (heap.size() < maxCandidates) {
                    heap.push_back(
                        HeapEntry{std::move(sortKey), key, std::make_unique<ValueType>(entry)});
                    std::push_heap(heap.begin(), heap.end(), heapComp);
                } else if (isBetter(sortKey, heap.front().sortKey)) {
                    // Replace the worst candidate: move it to the back, overwrite it, sift up.
                    std::pop_heap(heap.begin(), heap.end(), heapComp);
                    heap.back() =
                        HeapEntry{std::move(sortKey), key, std::make_unique<ValueType>(entry)};
                    std::push_heap(heap.begin(), heap.end(), heapComp);
                }
            }
        }

        // Moves the heap entries into the returned result vector.
        std::vector<std::pair<KeyType, ValueType>> candidates;
        candidates.reserve(heap.size());
        for (auto& node : heap) {
            candidates.emplace_back(std::move(node.key), std::move(*node.value));
        }
        return candidates;
    }

    /**
     * Remove the entry with the 'key' from the cache. If there is no entry for the given key in
     * the cache, this call is a no-op.
     */
    void remove(const KeyType& key) {
        _partitionedCache->erase(key);
    }

    /**
     * Remove all the entries for keys for which the predicate returns true. Return the number of
     * removed entries.
     */
    template <typename UnaryPredicate>
    [[MONGO_MOD_PUBLIC]] size_t removeIf(UnaryPredicate predicate) {
        size_t nRemoved = 0;
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);
            nRemoved += lockedPartition->removeIf(predicate);
        }
        return nRemoved;
    }

    /**
     * Remove *all* cache entries.
     */
    void clear() {
        _partitionedCache->clear();
    }

    /**
     * Reset total cache size. If the size is set to a smaller value than before, enough entries are
     * evicted in order to ensure that the cache fits within the new budget. Returns the number of
     * entries evicted.
     */
    size_t reset(size_t cacheSize) {
        size_t numEvicted = 0;
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);
            numEvicted += lockedPartition->reset(cacheSize / _numPartitions);
        }

        return numEvicted;
    }

    /**
     * Returns the size of the cache.
     * Used for testing.
     */
    size_t size() const {
        return _partitionedCache->size();
    }

    /**
     * Returns the number of partitions.
     */
    size_t numPartitions() const {
        return _numPartitions;
    }

    /**
     * Invoke `op` for each entry in the cache. Consistency across partitions is not guaranteed.
     */
    void forEach(const std::function<void(const KeyType&, const ValueType&)>& op) const {
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            auto lockedPartition = _partitionedCache->lockOnePartitionById(partitionId);

            for (auto&& [key, entry] : *lockedPartition) {
                op(key, entry);
            }
        }
    }

    /**
     * Allow iterating over partitions. The provided function is called for each partition. The
     * argument to the function is another function which can delay acquiring the implicitly locked
     * partition until it's needed.
     */
    void forEachPartition(const std::function<void(const std::function<Partition()>&)>& op) const {
        for (size_t partitionId = 0; partitionId < _numPartitions; ++partitionId) {
            op([&]() { return _partitionedCache->lockOnePartitionById(partitionId); });
        }
    }

    Partition getPartition(PartitionId partitionId) {
        return _partitionedCache->lockOnePartitionById(partitionId);
    }

private:
    std::size_t _numPartitions;
    std::unique_ptr<Partitioned<Lru, Partitioner>> _partitionedCache;
};

}  // namespace mongo
