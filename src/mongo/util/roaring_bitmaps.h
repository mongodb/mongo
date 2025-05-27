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

// Prevent macro redefinition warning.
#ifdef IS_BIG_ENDIAN
#undef IS_BIG_ENDIAN
#endif
#include <roaring.hh>
#undef IS_BIG_ENDIAN

#include "mongo/util/assert_util.h"

#include <absl/container/btree_map.h>
#include <absl/container/flat_hash_set.h>

namespace mongo {
/**
 * Roaring Bitmaps implementation for 64 bit integers. It uses B-Tree map to store 32-bits roaring
 * bitmaps for memory efficiency.
 */
class Roaring64BTree {
private:
    class Iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using value_type = uint64_t;
        using reference = uint64_t&;
        using pointer = uint64_t*;

        struct EndTag {};

        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        Iterator(Iterator&&) = default;
        Iterator& operator=(Iterator&&) = default;

        // Constructor
        Iterator(const absl::btree_map<uint32_t, roaring::Roaring>* roarings)
            : _roarings(roarings),
              _treeIt(_roarings->begin()),
              _currentRoaring(&_defaultRoaring),
              _roaringIt(_defaultRoaring.begin()) {
            // Update _currentRoaring and _roaringIt if there are Roarings in the map.
            if (!isTreeExhausted()) {
                _currentRoaring = &_treeIt->second;
                _roaringIt = _currentRoaring->begin();
            }
        }

        Iterator(const absl::btree_map<uint32_t, roaring::Roaring>* roarings, const EndTag&)
            : _roarings(roarings),
              _treeIt(_roarings->end()),
              _currentRoaring(&_defaultRoaring),
              _roaringIt(_defaultRoaring.end()) {
            if (!isTreeExhausted()) {
                _currentRoaring = &_treeIt->second;
                _roaringIt = _currentRoaring->end();
            }
        }

        friend bool operator==(const Iterator& lhs, const Iterator& rhs) {
            uassert(9774504,
                    "Comparing iterators from two different Roaring64BTree",
                    lhs._roarings == rhs._roarings);

            if (lhs.isTreeExhausted() != rhs.isTreeExhausted() ||
                lhs.isRoaringExhausted() != rhs.isRoaringExhausted()) {
                return false;
            }

            if (!lhs.isTreeExhausted() && lhs._treeIt != rhs._treeIt) {
                return false;
            }

            if (!lhs.isRoaringExhausted() && lhs._roaringIt != rhs._roaringIt) {
                return false;
            }
            return true;
        };

        friend bool operator!=(const Iterator& lhs, const Iterator& rhs) {
            return !(lhs == rhs);
        }

        // ++it
        Iterator& operator++() {
            if (!isRoaringExhausted()) {
                ++_roaringIt;
            }
            if (isRoaringExhausted()) {
                if (!isTreeExhausted()) {
                    ++_treeIt;
                }

                if (!isTreeExhausted()) {
                    _currentRoaring = &_treeIt->second;
                    _roaringIt = _currentRoaring->begin();
                }
            }

            return *this;
        };

        value_type operator*() const {
            return getCurrentValue();
        }

    private:
        const absl::btree_map<uint32_t, roaring::Roaring>* _roarings;
        absl::btree_map<uint32_t, roaring::Roaring>::const_iterator _treeIt;

        // I need this to silence UBSAN.
        inline static const roaring::Roaring _defaultRoaring{};

        const roaring::Roaring* _currentRoaring;
        roaring::Roaring::const_iterator _roaringIt;

        bool isTreeExhausted() const {
            return _treeIt == _roarings->end();
        }

        bool isRoaringExhausted() const {
            return _roaringIt == _currentRoaring->end();
        }

        uint64_t getCurrentValue() const {
            uassert(9774500, "Dereferencing invalid Roaring64BTree Iterator", !isTreeExhausted());
            uassert(
                9774501, "Dereferencing invalid Roaring64BTree Iterator", !isRoaringExhausted());

            uint64_t high_value = (static_cast<uint64_t>(_treeIt->first) << 32);
            uint64_t low_value = static_cast<uint64_t>(*_roaringIt);
            return (high_value + low_value);
        }
    };

public:
    /**
     * Add the value to the bitmap. Returns true if a new value was added, false otherwise.
     */
    bool addChecked(uint64_t value) {
        if (get(highBytes(value)).addChecked(lowBytes(value))) {
            increaseBytesSize(value);
            return true;
        }
        return false;
    }

    /**
     * Add the value to the bitmap.
     */
    void add(uint64_t value) {
        addChecked(value);
    }

    /**
     * Return true if the value is in the set, false otherwise.
     */
    bool contains(uint64_t value) const {
        return _roarings.contains(highBytes(value))
            ? _roarings.at(highBytes(value)).contains(lowBytes(value))
            : false;
    }

    void clear() {
        _roarings.clear();
    }

    bool empty() const {
        return _roarings.empty();
    }

    uint64_t getApproximateSize() const {
        return _roarings.size() * sizeof(uint32_t) + _approximateRoaringsSize;
    }

    /* This method uses statistics provided by roaring to compute a more accurate memory consumption
     * but should be used less frequently since it traverses the whole structure in order to compute
     * the statistics. complexity: O(N*M), N the number of keys in _roarings, M the number of
     * containers in each roaring.*/
    uint64_t computeMemorySize() const {
        uint64_t memoryUsage{0};
        for (const auto& roaring : _roarings) {
            memoryUsage += (sizeof(uint32_t) + computeRoaringMemorySize(&roaring.second.roaring));
        }

        return memoryUsage;
    }

    /* Creates an iterator on the Roaring64BTree. The iterator iterates the elements in the
     * Roaring64BTree in ascending order.*/
    Iterator begin() const {
        return Iterator{&_roarings};
    }

    Iterator end() const {
        return Iterator{&_roarings, Iterator::EndTag{}};
    }

    typedef Iterator const_iterator;

private:
    static constexpr uint32_t highBytes(const uint64_t in) {
        return uint32_t(in >> 32);
    }

    static constexpr uint32_t lowBytes(const uint64_t in) {
        return uint32_t(in);
    }

    // This method should be called when a new recordId is added to increase the
    // _approximateRoaringsSize.
    void increaseBytesSize(uint64_t value) {
        // roaring uses the highest 16 bits as indexes to containers. In the containers it stores
        // the lowest 16 bits. According to the paper each container should not use more than
        // 16bits/integer and the whole container should not use more than 8KB.

        // 2 bytes added because a new RecordId was added in a container.
        _approximateRoaringsSize += 2;

        // The key used to track a container in a roaring array. It is the value with the last 16
        // bits zet to 0.
        const uint64_t mask = 0xFFFFFFFFFFFF0000ULL;
        const uint16_t containerKey = value & mask;

        // If the key has not been seen in the past, a new container has been created adding an
        // entry in the roaring array (i.e. 2 bytes). Additionally, a new entry will be added in the
        // local hashset (i.e. 4 more bytes).
        _approximateRoaringsSize += static_cast<uint64_t>(
            (2 + sizeof(uint64_t)) * (_existingContainers.insert(containerKey).second ? 1 : 0));
    }

    uint64_t computeRoaringMemorySize(const roaring::api::roaring_bitmap_t* r) const {
        roaring::api::roaring_statistics_t stats;
        roaring::api::roaring_bitmap_statistics(r, &stats);
        return stats.n_bytes_array_containers + stats.n_bytes_bitset_containers +
            stats.n_bytes_run_containers;
    }

    roaring::Roaring& get(uint32_t key) {
        roaring::Roaring& roaring = _roarings[key];
        roaring.setCopyOnWrite(false);
        return roaring;
    }

    absl::btree_map<uint32_t, roaring::Roaring> _roarings;

    // Approximate size in bytes of the roarings.
    uint64_t _approximateRoaringsSize{0};
    absl::flat_hash_set<uint64_t> _existingContainers;
};

}  // namespace mongo
