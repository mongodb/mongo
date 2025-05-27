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

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <climits>
#include <compare>
#include <initializer_list>
#include <ostream>
#include <type_traits>

namespace mongo {
/**
 * An InlinedStorage is a simplifed version of `absl::InlinedVector`, and optimized for perfomance
 * and minimal overhead. It has the capability to inline a maximum of 'inlinedCapacity' elements of
 * the 'BT' type. When the number of elements exceeds the 'inlinedCapacity' threshold, the data is
 * then stored in the heap.
 */
template <typename BT,
          size_t inlinedCapacity,
          std::enable_if_t<std::is_trivial_v<BT> && (inlinedCapacity > 0), int> = 0>
class InlinedStorage {
public:
    using value_type = BT;
    using iterator = value_type*;
    using const_iterator = const value_type*;

    explicit InlinedStorage(size_t size) : _size(size) {
        if (MONGO_unlikely(!_isInlined())) {
            _buffer.onHeap = new value_type[_size];
            std::fill_n(_buffer.onHeap, _size, value_type(0));
        } else {
            std::fill_n(_buffer.inlined, inlinedCapacity, value_type(0));
        }
    }

    InlinedStorage(std::initializer_list<value_type> blocks) : _size(blocks.size()) {
        if (MONGO_unlikely(!_isInlined())) {
            _buffer.onHeap = new value_type[_size];
            std::copy_n(blocks.begin(), _size, _buffer.onHeap);
        } else {
            std::fill_n(
                std::copy_n(blocks.begin(), _size, _buffer.inlined), inlinedCapacity - _size, 0);
        }
    }

    InlinedStorage(const InlinedStorage& other) : _size(other._size) {
        if (MONGO_likely(_isInlined())) {
            _copyInlinedData(other);
        } else {
            _buffer.onHeap = new value_type[_size];
            std::copy_n(other._buffer.onHeap, _size, _buffer.onHeap);
        }
    }

    InlinedStorage& operator=(const InlinedStorage& other) {
        if (this != &other) {
            if (_size == other._size) {
                if (MONGO_likely(_isInlined())) {
                    _copyInlinedData(other);
                } else {
                    std::copy_n(other._buffer.onHeap, _size, _buffer.onHeap);
                }
            } else {
                if (MONGO_unlikely(!_isInlined())) {
                    delete[] _buffer.onHeap;
                }

                _size = other._size;

                if (MONGO_likely(_isInlined())) {
                    _copyInlinedData(other);
                } else {
                    _buffer.onHeap = new value_type[_size];
                    std::copy_n(other._buffer.onHeap, _size, _buffer.onHeap);
                }
            }
        }

        return *this;
    }

    InlinedStorage(InlinedStorage&& other) noexcept
        : _buffer(other._buffer), _size(std::exchange(other._size, 0)) {}

    InlinedStorage& operator=(InlinedStorage&& other) noexcept {
        if (this != &other) {
            if (MONGO_unlikely(!_isInlined())) {
                delete[] _buffer.onHeap;
            }
            _size = std::exchange(other._size, 0);
            _buffer = other._buffer;
        }
        return *this;
    }

    ~InlinedStorage() {
        if (MONGO_unlikely(!_isInlined())) {
            delete[] _buffer.onHeap;
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE iterator begin() {
        return data();
    }
    MONGO_COMPILER_ALWAYS_INLINE iterator end() {
        return data() + size();
    }
    MONGO_COMPILER_ALWAYS_INLINE const_iterator begin() const {
        return data();
    }
    MONGO_COMPILER_ALWAYS_INLINE const_iterator end() const {
        return data() + size();
    }

    MONGO_COMPILER_ALWAYS_INLINE value_type& operator[](size_t index) {
        return data()[index];
    }

    MONGO_COMPILER_ALWAYS_INLINE const value_type& operator[](size_t index) const {
        return data()[index];
    }

    MONGO_COMPILER_ALWAYS_INLINE value_type* data() {
        if (MONGO_likely(_isInlined())) {
            return _buffer.inlined;
        } else {
            return _buffer.onHeap;
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE MONGO_COMPILER_RETURNS_NONNULL const value_type* data() const {
        if (MONGO_likely(_isInlined())) {
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
            _shrink(newSize);
        } else {
            _grow(newSize);
        }
    }

    template <typename H>
    friend H AbslHashValue(H h, const InlinedStorage& storage) {
        return H::combine_contiguous(std::move(h), storage.data(), storage.size());
    }

    bool operator==(const InlinedStorage& rhs) const {
        return size() == rhs.size() && std::equal(data(), data() + size(), rhs.data());
    }

    auto operator<=>(const InlinedStorage& o) const {
        // XXX: use `std::lexicographical_three_way` when available
        auto a0 = data();
        auto a1 = data() + size();
        auto b0 = o.data();
        auto b1 = o.data() + o.size();
        for (;; a0++, b0++) {
            int aMore = (a0 != a1);
            int bMore = (b0 != b1);
            if (!aMore || !bMore)
                return aMore <=> bMore;
            if (auto cmp = *a0 <=> *b0; cmp != 0)
                return cmp;
        }
    }

    friend std::ostream& operator<<(std::ostream& os, const InlinedStorage& storage) {
        StringData sep;
        os << "[";
        for (auto& e : storage) {
            os << sep << e;
            sep = ", "_sd;
        }
        os << "]";
        return os;
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE bool _isInlined() const noexcept {
        return _willBeInlined(_size);
    }

    MONGO_COMPILER_ALWAYS_INLINE bool _willBeInlined(size_t size) const noexcept {
        return size <= inlinedCapacity;
    }

    void _copyInlinedData(const InlinedStorage& other) {
        if constexpr (inlinedCapacity == 1) {
            _buffer.inlined[0] = other._buffer.inlined[0];
        } else if constexpr (inlinedCapacity == 2) {
            _buffer.inlined[0] = other._buffer.inlined[0];
            _buffer.inlined[1] = other._buffer.inlined[1];
        } else {
            std::copy_n(other._buffer.inlined, _size, _buffer.inlined);
        }
    }

    void _shrink(size_t newSize) {
        // Inlined -> Inlined: do nothing, just update the size.
        // On heap -> on heap. Do nothing, just update the size and forget that we actually
        // allocated more memory than we use. Here we trade off memory for CPU.

        if (!_isInlined() && _willBeInlined(newSize)) {
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

    void _grow(size_t newSize) {
        if (_isInlined()) {
            if (_willBeInlined(newSize)) {
                // Inlined -> Inlined. Update the size, and zero new memory.
                std::fill(_buffer.inlined + _size, _buffer.inlined + newSize, value_type(0));
            } else {
                // Inlined -> on heap. Copy the data and free newly added memory.
                value_type* buffer = new value_type[newSize];
                // Copy old blocks.
                std::copy_n(_buffer.inlined, _size, buffer);

                _buffer.onHeap = buffer;

                // Zero newly added blocks.
                std::fill(_buffer.onHeap + _size, _buffer.onHeap + newSize, value_type(0));
            }
        } else {
            // On heap -> Allocated: Allocate new buffer, copy the data, zero new
            // data, free the old buffer.
            value_type* newBuffer = new value_type[newSize];
            // Copy old blocks.
            std::copy(_buffer.onHeap, _buffer.onHeap + _size, newBuffer);
            delete[] _buffer.onHeap;

            _buffer.onHeap = newBuffer;

            // Zero newly added blocks.
            std::fill(_buffer.onHeap + _size, _buffer.onHeap + newSize, value_type(0));
        }

        _size = newSize;
    }

    MONGO_COMPILER_ALWAYS_INLINE void _assertSizeEq(const InlinedStorage& other) const {
        if constexpr (kDebugBuild) {
            fassert(8271603, size() == other.size());
        }
    }

    union {
        value_type inlined[inlinedCapacity];
        value_type* onHeap;
    } _buffer;
    size_t _size;
};
}  // namespace mongo
