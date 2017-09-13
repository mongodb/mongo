/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include <boost/align/aligned_allocator.hpp>

#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/with_alignment.h"

namespace mongo {

inline std::size_t partitionOf(const char x, const std::size_t nPartitions) {
    return static_cast<unsigned char>(x) % nPartitions;
}
inline std::size_t partitionOf(const unsigned char x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const signed char x, const std::size_t nPartitions) {
    return static_cast<unsigned char>(x) % nPartitions;
}
inline std::size_t partitionOf(const int x, const std::size_t nPartitions) {
    return static_cast<unsigned int>(x) % nPartitions;
}
inline std::size_t partitionOf(const unsigned int x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const short x, const std::size_t nPartitions) {
    return static_cast<unsigned short>(x) % nPartitions;
}
inline std::size_t partitionOf(const unsigned short x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const long x, const std::size_t nPartitions) {
    return static_cast<unsigned long>(x) % nPartitions;
}
inline std::size_t partitionOf(const unsigned long x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const long long x, const std::size_t nPartitions) {
    return static_cast<unsigned long long>(x) % nPartitions;
}
inline std::size_t partitionOf(const unsigned long long x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const wchar_t x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const char16_t x, const std::size_t nPartitions) {
    return x % nPartitions;
}
inline std::size_t partitionOf(const char32_t x, const std::size_t nPartitions) {
    return x % nPartitions;
}

/**
 * The default partitioning policy: If using a numeric built-in type, will use the lower bits of a
 * number to decide which partition to assign it to. If using any other type T, you must define
 * partitionOf(const T&, std::size_t) or specialize this template.
 */
template <typename T>
struct Partitioner {
    std::size_t operator()(const T& x, const std::size_t nPartitions) {
        return partitionOf(x, nPartitions);
    }
};

namespace partitioned_detail {

using CacheAlignedMutex = CacheAligned<stdx::mutex>;

template <typename Key, typename Value>
Key getKey(const std::pair<Key, Value>& pair) {
    return std::get<0>(pair);
}

template <typename Key>
Key getKey(const Key& key) {
    return key;
}

template <typename T>
inline std::vector<stdx::unique_lock<stdx::mutex>> lockAllPartitions(T& mutexes) {
    std::vector<stdx::unique_lock<stdx::mutex>> result;
    result.reserve(mutexes.size());
    std::transform(mutexes.begin(), mutexes.end(), std::back_inserter(result), [](auto&& mutex) {
        return stdx::unique_lock<stdx::mutex>{mutex};
    });
    return result;
}
}  // namespace partitioned_detail

/**
 * A templated class used to partition an associative container like a set or a map to increase
 * scalability. `AssociativeContainer` is a type like a std::map or std::set that meets the
 * requirements of either the AssociativeContainer or UnorderedAssociativeContainer concept.
 * `nPartitions` determines how many partitions to make. `Partitioner` can be provided to customize
 * how the partition of each entry is computed.
 */
template <typename AssociativeContainer,
          std::size_t nPartitions = 16,
          typename KeyPartitioner = Partitioner<typename AssociativeContainer::key_type>>
class Partitioned {
private:
    // Used to create an iterator representing the end of the partitioned structure.
    struct IteratorEndTag {};

public:
    static_assert(nPartitions > 0, "cannot create partitioned structure with 0 partitions");
    using value_type = typename AssociativeContainer::value_type;
    using key_type = typename AssociativeContainer::key_type;
    using PartitionId = std::size_t;

    /**
     * Used to protect access to all partitions of this partitioned associative structure. For
     * example, may be used to empty each partition in the structure, or to provide a snapshotted
     * count of the number of entries across all partitions.
     */
    class All {
    private:
        /**
         * Acquires locks for all partitions. The lifetime of this `All` object must be shorter than
         * that of `partitionedContainer`.
         */
        explicit All(Partitioned& partitionedContainer)
            : _lockGuards(partitioned_detail::lockAllPartitions(partitionedContainer._mutexes)),
              _partitionedContainer(&partitionedContainer) {}

    public:
        /**
         * Returns an iterator at the start of the partitions.
         */
        auto begin() & {
            return this->_partitionedContainer->_partitions.begin();
        }

        /**
         * Returns an iterator at the end of the partitions.
         */
        auto end() & {
            return this->_partitionedContainer->_partitions.end();
        }

        /**
         * Returns an iterator at the start of the partitions.
         */
        auto begin() const& {
            return this->_partitionedContainer->_partitions.begin();
        }
        void begin() && = delete;

        /**
         * Returns an iterator at the end of the partitions.
         */
        auto end() const& {
            return this->_partitionedContainer->_partitions.end();
        }
        void end() && = delete;

        /**
         * Returns the number of elements in all partitions, summed together.
         */
        std::size_t size() const {
            return std::accumulate(
                this->_partitionedContainer->_partitions.begin(),
                this->_partitionedContainer->_partitions.end(),
                std::size_t{0},
                [](auto&& total, auto&& partition) { return total + partition.size(); });
        }

        /**
         * Returns true if each partition is empty.
         */
        bool empty() const {
            return std::all_of(this->_partitionedContainer->_partitions.begin(),
                               this->_partitionedContainer->_partitions.end(),
                               [](auto&& partition) { return partition.empty(); });
        }

        /**
         * Returns the number of entries with the given key.
         */
        std::size_t count(const key_type& key) const {
            auto partitionId = KeyPartitioner()(key, nPartitions);
            return this->_partitionedContainer->_partitions[partitionId].count(key);
        }

        /**
         * Empties each container within each partition.
         */
        void clear() {
            for (auto&& partition : this->_partitionedContainer->_partitions) {
                partition.clear();
            }
        }

        /**
         * Inserts `value` into its designated partition.
         */
        void insert(value_type value) & {
            const auto partitionId =
                KeyPartitioner()(partitioned_detail::getKey(value), nPartitions);
            this->_partitionedContainer->_partitions[partitionId].insert(std::move(value));
        }
        void insert(value_type)&& = delete;

        /**
         * Erases one entry from the partitioned structure, returns the number of entries removed.
         */
        std::size_t erase(const key_type& key) & {
            const auto partitionId = KeyPartitioner()(key, nPartitions);
            return this->_partitionedContainer->_partitions[partitionId].erase(key);
        }
        void erase(const key_type&) && = delete;

    private:
        friend class Partitioned;

        std::vector<stdx::unique_lock<stdx::mutex>> _lockGuards;
        Partitioned* _partitionedContainer;
    };

    /**
     * Used to protect access to a single partition of a Partitioned. For example, can be used to do
     * a series of reads and/or modifications to a single entry without interference from other
     * threads.
     */
    class OnePartition {
    public:
        /**
         * Returns a pointer to the structure in this partition.
         */
        AssociativeContainer* operator->() const& {
            return &this->_partitioned->_partitions[_id];
        }
        void operator->() && = delete;

        /**
         * Returns a reference to the structure in this partition.
         */
        AssociativeContainer& operator*() const& {
            return this->_partitioned->_partitions[_id];
        }
        void operator*() && = delete;

    private:
        friend class Partitioned;

        /**
         * Acquires locks for the ith partition.  `partitionedAssociativeContainer` must outlive
         * this GuardedPartition. If a single thread needs access to multiple partitions, it must
         * use GuardedAssociativeContainer, or acquire them in ascending order.
         */
        OnePartition(Partitioned& partitioned, PartitionId partitionId)
            : _partitionLock(partitioned._mutexes[partitionId]),
              _partitioned(&partitioned),
              _id(partitionId) {}

        stdx::unique_lock<stdx::mutex> _partitionLock;
        Partitioned* _partitioned;
        PartitionId _id;
    };

    /**
     * Constructs a partitioned version of a AssociativeContainer, with `nPartitions` partitions.
     */
    Partitioned() : _mutexes(nPartitions), _partitions(nPartitions) {}

    Partitioned(const Partitioned&) = delete;
    Partitioned(Partitioned&&) = default;
    Partitioned& operator=(const Partitioned&) = delete;
    Partitioned& operator=(Partitioned&&) = default;
    ~Partitioned() = default;

    /**
     * Returns true if each partition is empty. Locks the all partitions to perform this check, but
     * insertions can occur as soon as this method returns.
     */
    bool empty() const {
        const auto all = partitioned_detail::lockAllPartitions(this->_mutexes);
        return std::all_of(this->_partitions.begin(),
                           this->_partitions.end(),
                           [](auto&& partition) { return partition.empty(); });
    }

    /**
     * Returns the number of elements in all partitions, summed together.  Locks all partitions to
     * do this computation, but the size can change as soon as this method returns.
     */
    std::size_t size() const {
        const auto all = partitioned_detail::lockAllPartitions(this->_mutexes);
        return std::accumulate(
            this->_partitions.begin(),
            this->_partitions.end(),
            std::size_t{0},
            [](auto&& total, auto&& partition) { return total + partition.size(); });
    }

    /**
     * Returns the number of entries with the given key. Acquires locks for only the partition
     * determined by that key.
     */
    std::size_t count(const key_type& key) & {
        auto partition = this->lockOnePartition(key);
        return partition->count(key);
    }
    void count(const key_type&) && = delete;

    /**
     * Empties all partitions.
     */
    void clear() {
        auto all = this->lockAllPartitions();
        all.clear();
    }

    /**
     * Inserts a single value into the partitioned structure. Locks a single partition determined by
     * the value itself. Will not lock any partitions besides the one inserted into.
     */
    void insert(const value_type value) & {
        auto partition = this->lockOnePartitionById(
            KeyPartitioner()(partitioned_detail::getKey(value), nPartitions));
        partition->insert(std::move(value));
    }
    void insert(const value_type) && = delete;

    /**
     * Erases one entry from the partitioned structure. Locks only the partition given by the key.
     * Returns the number of entries removed.
     */
    std::size_t erase(const key_type& key) & {
        auto partition = this->lockOnePartition(key);
        return partition->erase(key);
    }
    void erase(const key_type&) && = delete;

    All lockAllPartitions() & {
        return All{*this};
    }

    OnePartition lockOnePartition(const key_type key) & {
        return OnePartition{*this, KeyPartitioner()(key, nPartitions)};
    }

    OnePartition lockOnePartitionById(PartitionId id) & {
        return OnePartition{*this, id};
    }

private:
    using CacheAlignedAssociativeContainer = CacheAligned<AssociativeContainer>;

    template <typename T>
    using AlignedVector = std::vector<T, boost::alignment::aligned_allocator<T>>;

    // These two vectors parallel each other, but we keep them separate so that we can return an
    // iterator over `_partitions` from within All.
    mutable AlignedVector<partitioned_detail::CacheAlignedMutex> _mutexes;
    AlignedVector<CacheAlignedAssociativeContainer> _partitions;
};
}  // namespace mongo
