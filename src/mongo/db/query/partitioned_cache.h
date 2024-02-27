/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/container_size_helper.h"

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
    size_t removeIf(UnaryPredicate predicate) {
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
