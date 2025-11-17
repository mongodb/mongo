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

#include <bitset>

/**
 * Defines the iterable function which provides simple iteration over the indices of the set bits in
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
 * If the largest set bit index is known, you can provide it to optimize the iteration range:
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
 */
namespace mongo {
/**
 * A forward iterator that iterates over the indexes of the set bits of the given bitset.
 */
template <size_t N>
class BitsetIterator {
public:
    using value_type = size_t;
    using difference_type = std::ptrdiff_t;

    BitsetIterator() : _bitset(0), _index(N), _size(N) {}

    explicit BitsetIterator(const std::bitset<N>& bitset, size_t index, size_t size)
        : _bitset(bitset), _index(index), _size(size) {
        move();
    }

    size_t operator*() const {
        return _index;
    }

    BitsetIterator& operator++() {
        ++_index;
        move();
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

    bool operator!=(const BitsetIterator& other) const {
        return !(*this == other);
    }

private:
    std::bitset<N> _bitset;
    std::size_t _index;
    // Claim that the index of the largest set bit is guaranteed to be (_size - 1) or less.
    std::size_t _size;

    void move() {
        for (; _index < _size && !_bitset[_index]; ++_index)
            ;
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
 * An iterable wrapper around bitset.
 */
template <size_t N>
class IterableBitset {
public:
    explicit IterableBitset(const std::bitset<N>& bitset, size_t size)
        : _bitset(bitset), _size(size) {}

    BitsetIterator<N> begin() const {
        return BitsetIterator<N>{_bitset, 0, _size};
    }

    BitsetIterator<N> end() const {
        return BitsetIterator<N>{_bitset, _size, _size};
    }

private:
    const std::bitset<N>& _bitset;
    // Claim that the index of the largest set bit is guaranteed to be (_size - 1) or less.
    size_t _size;
};

/**
 * A utility function that creates an iterable bitset.
 */
template <size_t N>
IterableBitset<N> iterable(const std::bitset<N>& bitset, size_t size = N) {
    return IterableBitset<N>{bitset, size};
}
}  // namespace mongo
