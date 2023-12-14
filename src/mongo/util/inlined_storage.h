/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <algorithm>
#include <climits>
#include <initializer_list>
#include <ostream>
#include <type_traits>

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * An InlinedStorage is a simplifed version of `absl::InlinedVector`, and optimized for perfomance
 * and minimal overhead. It has the capability to inline a maximum of 'InlinedCapacity' elements of
 * the 'BT' type. When the number of elements exceeds the 'InlinedCapacity' threshold, the data is
 * then stored in the heap.
 */
template <typename BT, size_t InlinedCapacity>
requires std::is_trivial_v<BT> &&(InlinedCapacity > static_cast<size_t>(0)) class InlinedStorage {
public:
    using BlockType = BT;
    static constexpr size_t kBlockSize = sizeof(BlockType) * CHAR_BIT;

    explicit InlinedStorage(size_t size) : _size(size) {
        if (!isInlined()) {
            _buffer.onHeap = new BlockType[_size];
            std::fill_n(_buffer.onHeap, _size, BlockType(0));
        }
    }

    InlinedStorage(std::initializer_list<BlockType> blocks) : InlinedStorage(blocks.size()) {
        std::copy(blocks.begin(), blocks.end(), data());
    }

    InlinedStorage(const InlinedStorage& other) : _size(other._size) {
        if (isInlined()) {
            copyInlinedData(other._buffer.inlined);
        } else {
            _buffer.onHeap = new BlockType[_size];
            std::copy(other._buffer.onHeap, other._buffer.onHeap + _size, _buffer.onHeap);
        }
    }

    InlinedStorage(InlinedStorage&& other) : _buffer(other._buffer), _size(other._size) {
        other._size = 0;
    }

    InlinedStorage& operator=(const InlinedStorage& other) {
        if (this == &other) {
            return *this;
        }

        if (_size == other._size) {
            if (MONGO_likely(isInlined())) {
                copyInlinedData(other._buffer.inlined);
            } else {
                std::copy(other._buffer.onHeap, other._buffer.onHeap + _size, _buffer.onHeap);
            }
        } else {
            if (MONGO_unlikely(!isInlined())) {
                delete[] _buffer.onHeap;
            }

            _size = other._size;

            if (MONGO_likely(isInlined())) {
                copyInlinedData(other._buffer.inlined);
            } else {
                _buffer.onHeap = new BlockType[_size];
                std::copy(other._buffer.onHeap, other._buffer.onHeap + _size, _buffer.onHeap);
            }
        }

        return *this;
    }

    InlinedStorage& operator=(InlinedStorage&& other) {
        if (this == &other) {
            return *this;
        }

        if (MONGO_unlikely(!isInlined())) {
            delete[] _buffer.onHeap;
        }

        _size = other._size;
        _buffer = other._buffer;

        other._size = 0;
        other._buffer.onHeap = nullptr;

        return *this;
    }

    ~InlinedStorage() {
        if (!isInlined()) {
            delete[] _buffer.onHeap;
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE BlockType& operator[](size_t index) {
        return data()[index];
    }

    MONGO_COMPILER_ALWAYS_INLINE const BlockType& operator[](size_t index) const {
        return data()[index];
    }

    MONGO_COMPILER_ALWAYS_INLINE BlockType* data() {
        if (isInlined()) {
            return _buffer.inlined;
        } else {
            return _buffer.onHeap;
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE MONGO_COMPILER_RETURNS_NONNULL const BlockType* data() const {
        if (isInlined()) {
            return _buffer.inlined;
        } else {
            return _buffer.onHeap;
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE size_t size() const noexcept {
        return _size;
    }

    /**
     * Resize the storage. Newly added memory is zeroed.
     */
    void resize(size_t newSize) {
        if (_size == newSize) {
            return;
        }

        if (_size > newSize) {
            shrink(newSize);
        } else {
            grow(newSize);
        }
    }

    template <typename H>
    friend H AbslHashValue(H h, const InlinedStorage& storage) {
        return H::combine_contiguous(std::move(h), storage.data(), storage.size());
    }

    /**
     * Returns true if the predicate returns true for all correspondong blocks of the storages.
     */
    template <typename Predicate, typename... Storage>
    friend bool allOf(Predicate predicate, const InlinedStorage& s, const Storage&... ss) {
        (s.assertSize(ss), ...);

        if (s.isInlined()) {
            for (size_t i = 0; i < s.size(); ++i) {
                if (!predicate(s._buffer.inlined[i], ss._buffer.inlined[i]...)) {
                    return false;
                }
            }
        } else {
            for (size_t i = 0; i < s.size(); ++i) {
                if (!predicate(s._buffer.onHeap[i], ss._buffer.onHeap[i]...)) {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * Returns true if the predicate returns true for att least a pair of correspondong blocks of
     * the storages.
     */
    template <class Predicate, typename... Storage>
    friend bool anyOf(Predicate predicate, const InlinedStorage& s, const Storage&... ss) {
        (s.assertSize(ss), ...);

        if (s.isInlined()) {
            for (size_t i = 0; i < s.size(); ++i) {
                if (predicate(s._buffer.inlined[i], ss._buffer.inlined[i]...)) {
                    return true;
                }
            }
        } else {
            for (size_t i = 0; i < s.size(); ++i) {
                if (predicate(s._buffer.onHeap[i], ss._buffer.onHeap[i]...)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE bool isInlined() const noexcept {
        return _size <= InlinedCapacity;
    }

    MONGO_COMPILER_ALWAYS_INLINE bool willBeInlined(size_t size) const noexcept {
        return size <= InlinedCapacity;
    }

    void copyInlinedData(const BlockType otherBits[InlinedCapacity]) {
        if constexpr (InlinedCapacity == 1) {
            _buffer.inlined[0] = otherBits[0];
        }
        if constexpr (InlinedCapacity == 2) {
            _buffer.inlined[0] = otherBits[0];
            _buffer.inlined[1] = otherBits[1];
        }
        if constexpr (InlinedCapacity > 2) {
            std::copy(otherBits, otherBits + _size, _buffer.inlined);
        }
    }

    void shrink(size_t newSize) {
        // Inlined -> Inlined: do nothing, just update the size.
        // On heap -> on heap. Do nothing, just update the size and forget that we actually
        // allocated more memory than we use. Here we trade off memory for CPU.

        if (!isInlined() && willBeInlined(newSize)) {
            // On heap -> Inlined: copy data to inlined and free the buffer.

            // Exception safety note: no exceptions are expected be raised in std::copy(),
            // so the buffer will be freed safely.
            auto oldBuffer = _buffer.onHeap;
            std::copy(oldBuffer, oldBuffer + newSize, _buffer.inlined);

            // No exceptions are expected here, otherwise we have to update '_size' before.
            delete[] oldBuffer;
        }

        _size = newSize;
    }

    void grow(size_t newSize) {
        if (isInlined()) {
            if (willBeInlined(newSize)) {
                // Inlined -> Inlined. Update the size, and zero new memory.
                std::fill(_buffer.inlined + _size, _buffer.inlined + newSize, BlockType(0));
            } else {
                // Inlined -> on heap. Copy the data and free newly added memory.
                BlockType* buffer = new BlockType[newSize];
                // Copy old blocks.
                std::copy(_buffer.inlined, _buffer.inlined + _size, buffer);

                _buffer.onHeap = buffer;

                // Zero newly added blocks.
                std::fill(_buffer.onHeap + _size, _buffer.onHeap + newSize, BlockType(0));
            }
        } else {
            // On heap -> Allocated: Allocate new buffer, copy the data, zero new
            // data, free the old buffer.
            BlockType* newBuffer = new BlockType[newSize];
            // Copy old blocks.
            std::copy(_buffer.onHeap, _buffer.onHeap + _size, newBuffer);
            delete[] _buffer.onHeap;

            _buffer.onHeap = newBuffer;

            // Zero newly added blocks.
            std::fill(_buffer.onHeap + _size, _buffer.onHeap + newSize, BlockType(0));
        }

        _size = newSize;
    }

    MONGO_COMPILER_ALWAYS_INLINE void assertSize(const InlinedStorage& other) const {
        if constexpr (kDebugBuild) {
            fassert(8271603, size() == other.size());
        }
    }

    union {
        BlockType inlined[InlinedCapacity]{0};
        BlockType* onHeap;
    } _buffer;
    size_t _size;
};

template <typename BT, size_t InlinedCapacity>
bool operator==(const InlinedStorage<BT, InlinedCapacity>& lhs,
                const InlinedStorage<BT, InlinedCapacity>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }

    return true;
}

/*
 * Return true if 'lhs' storage is lexicographically less than 'rhs', and false otherwise.
 */
template <typename BT, size_t InlinedCapacity>
bool operator<(const InlinedStorage<BT, InlinedCapacity>& lhs,
               const InlinedStorage<BT, InlinedCapacity>& rhs) {
    const size_t size = std::min(lhs.size(), rhs.size());

    for (size_t i = 0; i < size; ++i) {
        if (lhs[i] != rhs[i]) {
            return lhs[i] < rhs[i];
        }
    }

    return lhs.size() < rhs.size();
}

template <typename BT, size_t InlinedCapacity>
std::ostream& operator<<(std::ostream& os, const InlinedStorage<BT, InlinedCapacity>& storage) {
    os << "[";
    for (size_t i = 0; i < storage.size(); ++i) {
        if (i != 0) {
            os << ", ";
        }
        os << storage[i];
    }
    os << "]";

    return os;
}
}  // namespace mongo
