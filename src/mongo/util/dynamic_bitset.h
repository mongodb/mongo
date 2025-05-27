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

#include "mongo/base/string_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/inlined_storage.h"

#include <bit>
#include <type_traits>

namespace mongo {
namespace bitset_utils {
template <typename T>
T maskbit(size_t bitIndex) {
    return static_cast<T>(1) << bitIndex;
}
}  // namespace bitset_utils

/**
 * Bitset class implementation, which can dynamically grow and shrink. t has the capability to
 * inline up to (8 * sizeof(T) * nBlocks) bits When the number of elements exceeds the
 * threshold, the data is then stored in the heap. The size of the bitset is always a factor of 8 *
 * sizeof(T).
 */
template <typename T, size_t nBlocks, typename Storage = InlinedStorage<T, nBlocks>>
class DynamicBitset {
public:
    using BlockType = T;
    static_assert(std::is_integral_v<BlockType>);
    static_assert(nBlocks > 0);

    static constexpr size_t npos = static_cast<size_t>(-1);

    // Useful for bit operations constants.
    static constexpr BlockType kZero = 0;       // All bits unset: 0b00000000
    static constexpr BlockType kOnes = ~kZero;  // All bits set: 0b11111111

    /**
     * A proxy class serving as a reference to a bit.
     */
    class BitReference {
    public:
        BitReference(BlockType& block, size_t bitIndex) noexcept
            : _block(block), _bitIndex(bitIndex) {}

        BitReference& operator=(bool bit) noexcept {
            _set(bit);
            return *this;
        }

        BitReference& operator=(const BitReference& bit) noexcept {
            _set(bit);
            return *this;
        }

        /**
         * Return negated value of this bit.
         */
        bool operator~() const noexcept {
            return !*this;
        }

        /**
         * Return value of this bit.
         */
        operator bool() const noexcept {
            return _block & maskbit();
        }

        /**
         * Flip this bit.
         */
        BitReference& flip() noexcept {
            _block ^= maskbit();
            return *this;
        }

    private:
        BlockType maskbit() const noexcept {
            return bitset_utils::maskbit<BlockType>(_bitIndex);
        }

        void _set(bool b) noexcept {
            if (b) {
                _block |= maskbit();
            } else {
                _block &= ~maskbit();
            }
        }

        BlockType& _block;
        size_t _bitIndex;
    };

    /**
     * Allocates a bitset of default size. The bitset of default size occupies all available inlined
     * storage and never allocates. The default size is equal to nBlocks * sizeof(BlockType)
     * * CHAR_BIT. E.g., DynamicBitset<uint8_t, 1> has default size 8 bits, DynamicBitset<uint64_t,
     * 2> - 128 bits.
     */
    DynamicBitset() : _storage(nBlocks) {}

    /**
     * Create a bitset of size >= 'minSize'. The actual size will be ceil(minSize/sizeof(block)).
     * E.g., for 32 bit blocks (uint32_t) and the minSize equals 8 the actual size will be 32, for
     * minSize = 33, the actual size will be 64, and for minSize = 64 the actual size will be 64 as
     * well.
     */
    explicit DynamicBitset(size_t minSize) : _storage(getRequiredNumberOfBlocks(minSize)) {}

    /**
     * Create a bitset from a binary strings containing only '0's and '1's.
     */
    explicit DynamicBitset(StringData binaryString)
        : _storage(getRequiredNumberOfBlocks(binaryString.size())) {
        const size_t offset = binaryString.size() - 1;
        for (size_t i = 0; i < binaryString.size(); ++i) {
            fassert(8271600, binaryString[i] == '0' || binaryString[i] == '1');
            set(offset - i, binaryString[i] != '0');
        }
    }

    /**
     * Return true if at least one bit of this bitset is set.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool any() const {
        for (auto&& e : _storage)
            if (e)
                return true;
        return false;
    }

    /**
     * Return false if no bits of this bitset are set.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool none() const {
        return !any();
    }

    /**
     * Compute AND of this subset and subset 'other' and assign the result to this subset. The
     * bitsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator&=(const DynamicBitset& other) {
        assertSize(other);
        _forEach([](auto& e, const auto& o) { e &= o; }, other);
        return *this;
    }

    /**
     * Compute OR of this subset and subset 'other' and assign the result to this subset. The
     * bitsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator|=(const DynamicBitset& other) {
        assertSize(other);
        _forEach([](auto& a, const auto& b) { a |= b; }, other);
        return *this;
    }

    /**
     * Compute XOR of this subset and subset 'other' and assign the result to this subset. The
     * bitsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator^=(const DynamicBitset& other) {
        assertSize(other);
        _forEach([](auto& a, const auto& b) { a ^= b; }, other);
        return *this;
    }

    /**
     * Compute a set difference of this subset and subset 'other' and assign the result to this
     * bitsets. The subsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator-=(const DynamicBitset& other) {
        assertSize(other);
        _forEach([](auto& a, const auto& b) { a &= ~b; }, other);
        return *this;
    }

    /**
     * Flip all bits of this bitset.
     */
    MONGO_COMPILER_ALWAYS_INLINE void flip() {
        _forEach([](auto& e) { e = ~e; });
    }

    /**
     * Complement operator: Return a new bitset with all bits flipped.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset operator~() const {
        DynamicBitset result(*this);
        result.flip();
        return result;
    }

    /**
     * Return a reference to 'index'-th bit. Using the reference you may change read or set the bit.
     */
    MONGO_COMPILER_ALWAYS_INLINE BitReference operator[](size_t index) {
        assertBitIndex(index);
        return BitReference(_storage[getBlockIndex(index)], getBitIndex(index));
    }

    /**
     * Return a value of 'index'-th bit.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool operator[](size_t index) const {
        assertBitIndex(index);
        return _storage[getBlockIndex(index)] & maskbit(getBitIndex(index));
    }

    /**
     * Set all bits of this bitset.
     */
    MONGO_COMPILER_ALWAYS_INLINE void set() {
        std::fill(_storage.begin(), _storage.end(), kOnes);
    }

    /**
     * Set 'index'-th bit to the given 'value'.
     */
    MONGO_COMPILER_ALWAYS_INLINE void set(size_t index, bool value = true) {
        assertBitIndex(index);
        const auto blockIndex = getBlockIndex(index);
        const auto bitIndex = getBitIndex(index);
        _storage[blockIndex] &= ~maskbit(bitIndex);
        _storage[blockIndex] |= static_cast<BlockType>(value) << bitIndex;
    }

    /**
     * Return the number of set bits in the bitset.
     */
    MONGO_COMPILER_ALWAYS_INLINE size_t count() const {
        size_t count = 0;
        _forEach([&](auto&& e) { count += std::popcount(e); });
        return count;
    }

    /**
     * Return true if this bitset is a subset of subset 'other'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool isSubsetOf(const DynamicBitset& other) const {
        return allOf([](auto a, auto b) { return a == (a & b); }, *this, other);
    }

    /**
     * Return true if this bitset has common set bits with 'other'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool intersects(const DynamicBitset& other) const {
        return anyOf([](auto a, auto b) { return a & b; }, *this, other);
    }

    /**
     * Return true if this bitset is equal to masked 'other'. This operation is
     * implementation of 'this == (mask & other)'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool isEqualToMasked(const DynamicBitset& other,
                                                      const DynamicBitset& mask) const {
        return allOf([](auto a, auto b, auto m) { return a == (b & m); }, *this, other, mask);
    }

    /**
     * Return the index of first set bit, if nothing found return 'npos'.
     */
    size_t findFirst() const {
        for (size_t i = 0; i < _storage.size(); ++i) {
            const auto block = _storage[i];
            if (block != kZero) {
                return i * kBitsPerBlock + std::countr_zero(block);
            }
        }

        return npos;
    }

    /**
     * Return the index of the first set bit starting AFTER position 'previous', if nothing found
     * return 'npos'.
     */
    size_t findNext(size_t previous) const {
        const size_t size = this->size();
        if (++previous >= size) {
            return npos;
        }

        // Step 1. Checking the starting block.
        const size_t startBlockIndex = getBlockIndex(previous);
        const size_t startBitIndex = getBitIndex(previous);
        const BlockType startBlock = _storage[startBlockIndex] >> startBitIndex;
        if (startBlock != kZero) {
            return previous + std::countr_zero(startBlock);
        }

        // Step 2. In not found in the starting block, continue search as in 'findNext' function.
        for (size_t i = startBlockIndex + 1; i < _storage.size(); ++i) {
            const auto block = _storage[i];
            if (block != kZero) {
                return i * kBitsPerBlock + std::countr_zero(block);
            }
        }

        return npos;
    }

    /**
     * Resize this bitset. See the comment to 'DynamicBitset(size_t minSize)' about the actual size
     * of the bitset. All bits higher than 'newSize' will be cleared.
     */
    void resize(size_t newSize) {
        const bool shrinks = newSize < size();

        _storage.resize(getRequiredNumberOfBlocks(newSize));

        if (shrinks) {
            // Clear high bits.
            const size_t bitIndex = getBitIndex(newSize);
            const BlockType mask = static_cast<BlockType>(~(kOnes << bitIndex));
            _storage[_storage.size() - 1] &= mask;
        }
    }

    /**
     * Return the number of the bits in this subset.
     */
    MONGO_COMPILER_ALWAYS_INLINE size_t size() const {
        return _storage.size() * kBitsPerBlock;
    }

    /**
     * Iterates over integer blocks of the bitsets and returns true if the predicate returns true
     * for at least one set of corresponing blocks.
     */
    friend bool anyOf(auto predicate, const DynamicBitset& s, const auto&... ss) {
        return [&](auto... ssIters) {
            for (auto& sElem : s._storage)
                if (predicate(sElem, *ssIters++...))
                    return true;
            return false;
        }(ss._storage.begin()...);
    }

    /**
     * Iterates over integer blocks of the bitsets and returns true if the predicate returns true
     * for all sets of corresponing blocks.
     */
    friend bool allOf(auto predicate, const DynamicBitset& s, const auto&... ss) {
        return [&](auto... ssIters) {
            for (auto& sElem : s._storage)
                if (!predicate(sElem, *ssIters++...))
                    return false;
            return true;
        }(ss._storage.begin()...);
    }

    template <typename H>
    friend H AbslHashValue(H h, const DynamicBitset& bitset) {
        return H::combine(std::move(h), bitset._storage);
    }

    bool operator==(const DynamicBitset& rhs) const = default;
    auto operator<=>(const DynamicBitset& rhs) const = default;

    /** and */
    friend DynamicBitset operator&(DynamicBitset lhs, const DynamicBitset& rhs) {
        return lhs &= rhs;
    }

    /** or */
    friend DynamicBitset operator|(DynamicBitset lhs, const DynamicBitset& rhs) {
        return lhs |= rhs;
    }

    /** xor */
    friend DynamicBitset operator^(DynamicBitset lhs, const DynamicBitset& rhs) {
        return lhs ^= rhs;
    }

    /** Set difference. */
    friend DynamicBitset operator-(DynamicBitset lhs, const DynamicBitset& rhs) {
        return lhs -= rhs;
    }

    friend std::ostream& operator<<(std::ostream& os, const DynamicBitset& bitset) {
        for (size_t i = bitset.size(); i > 0; --i) {
            os << (bitset[i - 1] ? '1' : '0');
        }
        return os;
    }

private:
    static constexpr size_t kBitsPerBlock = sizeof(BlockType) * CHAR_BIT;

    static void _forEachImpl(auto&& self, auto&& f, auto&&... others) {
        return [&](auto... iters) {
            for (auto&& e : self._storage)
                f(e, *iters++...);
        }(others._storage.begin()...);
    }
    void _forEach(auto&& f, auto&&... others) const {
        _forEachImpl(*this, f, others...);
    }
    void _forEach(auto&& f, auto&&... others) {
        _forEachImpl(*this, f, others...);
    }


    MONGO_COMPILER_ALWAYS_INLINE static size_t getRequiredNumberOfBlocks(
        size_t sizeInBits) noexcept {
        return (sizeInBits + kBitsPerBlock - 1) / kBitsPerBlock;
    }

    MONGO_COMPILER_ALWAYS_INLINE static size_t getBlockIndex(size_t index) noexcept {
        return index / kBitsPerBlock;
    }

    MONGO_COMPILER_ALWAYS_INLINE static BlockType maskbit(size_t bitIndex) noexcept {
        return bitset_utils::maskbit<BlockType>(bitIndex);
    }

    /**
     * Returns bit inside inside its block.
     */
    MONGO_COMPILER_ALWAYS_INLINE static size_t getBitIndex(size_t index) noexcept {
        return index % kBitsPerBlock;
    }

    MONGO_COMPILER_ALWAYS_INLINE void assertSize(const DynamicBitset& other) const {
        if constexpr (kDebugBuild) {
            fassert(8271601, size() == other.size());
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE void assertBitIndex(size_t bitIndex) const {
        if constexpr (kDebugBuild) {
            fassert(8271602, bitIndex < size());
        }
    }

    // 0 is the least significant word.
    Storage _storage;
};

}  // namespace mongo
