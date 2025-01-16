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

#include "mongo/util/assert_util.h"

// Prevent macro redefinition warning.
#ifdef IS_BIG_ENDIAN
#undef IS_BIG_ENDIAN
#endif
#include <roaring.hh>
#undef IS_BIG_ENDIAN

#include <absl/container/btree_map.h>

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
     * Add the value to the bitmaps. Returns true if a new values was added, false otherwise.
     */
    bool addChecked(uint64_t value) {
        return get(highBytes(value)).addChecked(lowBytes(value));
    }

    /**
     * Add the value to the bitmaps.
     */
    void add(uint64_t value) {
        get(highBytes(value)).add(lowBytes(value));
    }

    /**
     * Return true if the value is in the set, false otherwise.
     */
    bool contains(uint64_t value) const {
        return _roarings.contains(highBytes(value))
            ? _roarings.at(highBytes(value)).contains(lowBytes(value))
            : false;
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

    roaring::Roaring& get(uint32_t key) {
        auto& roaring = _roarings[key];
        roaring.setCopyOnWrite(false);
        return roaring;
    }

    absl::btree_map<uint32_t, roaring::Roaring> _roarings;
};

}  // namespace mongo
