/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/util/modules.h"

#include <bit>
#include <bitset>
#include <climits>
#include <iterator>
#include <type_traits>

/**
 * Defines bitset utils:
 *
 * 1. The iterable function which provides simple iteration over the indices of the set bits in
 * the bitset.
 *
 * Usage:
 * #include <mongo/db/query/util/bitset_iterator.h>
 * #include <iostream>
 *
 * void test() {
 *     std::bitset<64> bitset;
 *     bitset.set(10);
 *     bitset.set(7);
 *     for (auto bitIndex : iterable(bitset)) {
 *         std::cout << bitIndex << std::endl;
 *     }
 *     // prints 7 and 10
 * }
 *
 * If the largest set bit index is known, you can provide it to hint the iteration range:
 *
 *  #include <mongo/db/query/util/bitset_iterator.h>
 *  #include <iostream>
 *
 *  void test() {
 *     constexpr size_t maxSetBit = 16;
 *     std::bitset<64> bitset;
 *     bitset.set(10);
 *     bitset.set(7);
 *     for (auto bitIndex : iterable(bitset, maxSetBit)) {
 *         std::cout << bitIndex << std::endl;
 *     }
 *     // prints 7 and 10
 *
 * 2. The comparison function of two bitsets: bitsetLess.
 */
namespace mongo {

/**
 * The number of bits that can be returned by 'std::bitset<N>::to_ullong()' function. If the
 * bitset's size is less or equal that number of bits we can get its underlaying data using the
 * 'to_ullong()' function to implement optimized access to the bits.
 */
constexpr std::size_t kBitsFitInULLong = sizeof(unsigned long long) * CHAR_BIT;

/**
 * A forward iterator that iterates over the indexes of the set bits of the given bitset.
 */
template <size_t N, typename Enable = void>
class BitsetIterator;

/**
 * A generic, non-effective specialization for large (>64) bitsets of BitsetIterator, a forward
 * iterator that iterates over the indexes of the set bits of the given bitset.
 */
template <size_t N>
class BitsetIterator<N, typename std::enable_if_t<(N > kBitsFitInULLong)>> {
public:
    using value_type = size_t;
    using difference_type = std::ptrdiff_t;

    BitsetIterator() = default;

    /**
     * Creates a new insrance of BitsetIterator.
     * index == size means end iterator.
     */
    BitsetIterator(const std::bitset<N>& bitset, size_t index, size_t size)
        : _bitset(bitset), _index(index), _size(size) {
        _move();
    }

    size_t operator*() const {
        return _index;
    }

    BitsetIterator& operator++() {
        ++_index;
        _move();
        return *this;
    }

    BitsetIterator operator++(int) {
        auto tmp{*this};
        ++(*this);
        return tmp;
    }

    bool operator==(const BitsetIterator& other) const {
        return _index == other._index && _bitset == other._bitset;
    }

private:
    std::bitset<N> _bitset{};
    std::size_t _index{N};
    // Claim that the index of the largest set bit is guaranteed to be (_size - 1) or less.
    std::size_t _size{N};

    void _move() {
        for (; _index < _size && !_bitset[_index]; ++_index)
            ;
    }
};

/**
 * An optimized specialization for small (<=64) bitsets of BitsetIterator, a forward
 * iterator that iterates over the indexes of the set bits of the given bitset.
 */
template <size_t N>
class BitsetIterator<N, typename std::enable_if_t<(N <= kBitsFitInULLong)>> {
public:
    using value_type = size_t;
    using difference_type = std::ptrdiff_t;

    BitsetIterator() = default;

    /**
     * Constructs a new insrance of BitsetIterator.
     * index == size means end iterator.
     */
    BitsetIterator(std::bitset<N> bitset, size_t index, size_t size)
        : _bitset(index < size ? bitset.to_ullong() : 0), _index(index), _size(size) {
        dassert(_index <= N, "index is expected to be less or equal the size of the bitset");
        _move();
    }

    size_t operator*() const {
        return _index;
    }

    BitsetIterator& operator++() {
        _move();
        return *this;
    }

    BitsetIterator operator++(int) {
        auto tmp{*this};
        ++(*this);
        return tmp;
    }

    bool operator==(const BitsetIterator& other) const {
        return _index == other._index && _bitset == other._bitset;
    }

private:
    unsigned long long _bitset{0};
    std::size_t _index{N};
    // Claim that the index of the largest set bit is guaranteed to be (_size - 1) or less.
    std::size_t _size{N};

    void _move() {
        if (_bitset != 0) {
            _index = std::countr_zero(_bitset);
            _bitset &= _bitset - 1;
        } else {
            _index = _size;
        }
    }
};

/**
 * Returns an iterator that positioned to the first set bit of the bitset.
 */
template <size_t N>
BitsetIterator<N> begin(const std::bitset<N>& bitset, size_t size = N) {
    return BitsetIterator<N>{bitset, 0, size};
}

/**
 * Returns an iterator that positioned after the last set bit of the bitset.
 */
template <size_t N>
BitsetIterator<N> end(const std::bitset<N>& bitset, size_t size = N) {
    return BitsetIterator<N>{bitset, size, size};
}

/**
 * A view on std::bitset provides a forward iterator over the indices of its set bits.
 * The iterator is invalidated after updating the bitset.
 */
template <size_t N>
class BitsetPopulationView {
public:
    explicit BitsetPopulationView(const std::bitset<N>& bitset, size_t size)
        : _bitset(bitset), _size(size) {}

    BitsetIterator<N> begin() const {
        return BitsetIterator<N>{_bitset, 0, _size};
    }

    BitsetIterator<N> end() const {
        return BitsetIterator<N>{_bitset, _size, _size};
    }

private:
    static_assert(std::forward_iterator<BitsetIterator<N>>);

    const std::bitset<N>& _bitset;
    // Claim that the index of the largest set bit is guaranteed to be (_size - 1) or less.
    size_t _size;
};

/**
 * A utility function that creates an iterable bitset.
 */
template <size_t N>
BitsetPopulationView<N> iterable(const std::bitset<N>& bitset, size_t size = N) {
    return BitsetPopulationView<N>{bitset, size};
}

/**
 * Returns true if the lhs bitset is smaller than the rhs in lexicographical order.
 */
template <size_t N>
bool bitsetLess(const std::bitset<N>& lhs, const std::bitset<N>& rhs) {
    if constexpr (N <= kBitsFitInULLong) {
        return lhs.to_ullong() < rhs.to_ullong();
    } else {
        if (lhs == rhs) {
            return false;
        }
        for (size_t i = N - 1; i >= 0; --i) {
            if (lhs[i] ^ rhs[i]) {
                return rhs[i];
            }
        }
        return false;
    }
}
}  // namespace mongo
