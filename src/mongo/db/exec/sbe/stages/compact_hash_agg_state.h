// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo::sbe {

/**
 * Typed offset of one accumulator state within a group's byte buffer. CompactStateLayout::add()
 * returns it, and the same position works for every group's buffer. This class exists to hide
 * reinterpret_cast implementation details. These states are constructed and destroyed in place
 * with std::construct_at and std::destroy_at.
 */
template <typename T>
class CompactStatePosition {
public:
    using value_type = T;

    CompactStatePosition() = default;

    value_type& operator()(std::byte* buffer) const {
        return *std::launder(_asValue(buffer, _offset));
    }

    const value_type& operator()(const std::byte* buffer) const {
        return *std::launder(_asValue(buffer, _offset));
    }

    size_t offset() const {
        return _offset;
    }

private:
    friend class CompactStateLayout;

    explicit CompactStatePosition(size_t offset) : _offset(offset) {}

    static value_type* _asValue(std::byte* buffer, size_t offset) {
        return reinterpret_cast<value_type*>(buffer + offset);
    }

    static const value_type* _asValue(const std::byte* buffer, size_t offset) {
        return reinterpret_cast<const value_type*>(buffer + offset);
    }

    static void construct(std::byte* buffer, size_t offset) {
        std::construct_at(_asValue(buffer, offset));
    }

    static void destroy(std::byte* buffer, size_t offset) {
        std::destroy_at(std::launder(_asValue(buffer, offset)));
    }

    size_t _offset = 0;
};

/**
 * This class defines the in-memory schema of a $group stage's accumulator states. This object
 * exists once per $group stage, not once per distinct group. New state entries are added with
 * add<T>(). Once every state is added, the layout moves into the CompactStateTable, which creates
 * and destroys each group's states from it.
 */
class CompactStateLayout {
public:
    /**
     * Add a default-constructible accumulator state of type T to the next position in the schema.
     * Returns the position of the state in each buffer.
     */
    template <typename T>
    CompactStatePosition<T> add() {
        // Ensure the new entry is aligned appropriately. Entries can have different alignments from
        // each other. Relies on alignments being powers of two.
        Entry entry{(_size + alignof(T) - 1) & ~(alignof(T) - 1),
                    &CompactStatePosition<T>::construct,
                    std::is_trivially_destructible_v<T> ? nullptr
                                                        : &CompactStatePosition<T>::destroy};
        _entries.push_back(entry);
        _triviallyDestructible = _triviallyDestructible && !entry.destroyFn;
        _size = entry.offset + sizeof(T);
        // The alignment of the layout is the greatest alignment of any individual entry.
        _alignment = std::max(_alignment, alignof(T));
        return CompactStatePosition<T>{entry.offset};
    }

    size_t size() const {
        return _size;
    }

    size_t alignment() const {
        return _alignment;
    }

    bool triviallyDestructible() const {
        return _triviallyDestructible;
    }

    size_t heapSize() const {
        return _entries.capacity() * sizeof(Entry);
    }

private:
    friend class CompactStateTable;

    struct Deleter;
    using OwnedStates = std::unique_ptr<std::byte, Deleter>;

    struct Entry {
        void construct(std::byte* buffer) const {
            constructFn(buffer, offset);
        }

        void destroy(std::byte* buffer) const {
            if (destroyFn) {
                destroyFn(buffer, offset);
            }
        }

        size_t offset;
        void (*constructFn)(std::byte*, size_t);
        void (*destroyFn)(std::byte*, size_t);
    };

    OwnedStates create() const;

    void destroy(std::byte* buffer) const {
        if (buffer) {
            destroy(buffer, _entries.size());
        }
    }

    void destroy(std::byte* buffer, size_t built) const;

    std::vector<Entry> _entries;
    size_t _size = 0;
    size_t _alignment = 1;
    bool _triviallyDestructible = true;
};

/**
 * The hash table structure used for compact SBE $group. It maps each group-by key to the buffer of
 * that group's accumulator states.
 */
class CompactStateTable {
public:
    using Map = absl::flat_hash_map<value::MaterializedRow,
                                    std::byte*,
                                    value::MaterializedRowHasher,
                                    value::MaterializedRowEq>;
    using iterator = Map::iterator;

    /**
     * The table owns 'layout' and uses it to create and destroy each group's states. 'hasher' and
     * 'eq' decide which group-by keys are the same group, for example under a collation.
     */
    explicit CompactStateTable(CompactStateLayout layout,
                               value::MaterializedRowHasher hasher = value::MaterializedRowHasher{},
                               value::MaterializedRowEq eq = value::MaterializedRowEq{})
        : _layout(std::move(layout)), _map(0, std::move(hasher), std::move(eq)) {}

    CompactStateTable(const CompactStateTable&) = delete;
    CompactStateTable& operator=(const CompactStateTable&) = delete;

    ~CompactStateTable() {
        clear();
    }

    const CompactStateLayout& layout() const {
        return _layout;
    }

    iterator findOrInsert(const value::MaterializedRow& key);

    void clear();

    iterator begin() {
        return _map.begin();
    }

    iterator end() {
        return _map.end();
    }

    size_t size() const {
        return _map.size();
    }

    bool empty() const {
        return _map.empty();
    }

private:
    const CompactStateLayout _layout;
    Map _map;
};

}  // namespace mongo::sbe
