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

#include <bit>
#include <type_traits>

#include "mongo/base/string_data.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/inlined_storage.h"

namespace mongo {
namespace bitset_utils {
template <typename T>
T maskbit(size_t bitIndex) {
    return static_cast<T>(1) << bitIndex;
}
}  // namespace bitset_utils

/**
 * Bitset class implementation, which can dynamically grow and shrink. t has the capability to
 * inline up to (8 * sizeof(T) * NumberOfBlocks) bits When the number of elements exceeds the
 * threshold, the data is then stored in the heap. The size of the bitset is always a factor of 8 *
 * sizeof(T).
 */
template <typename T, size_t NumberOfBlocks>
requires std::is_integral_v<T> &&(NumberOfBlocks > static_cast<size_t>(0)) class DynamicBitset {
public:
    using Storage = InlinedStorage<T, NumberOfBlocks>;
    using BlockType = T;

    static constexpr size_t kNpos = static_cast<size_t>(-1);

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

        BitReference& operator=(bool bitValue) noexcept {
            if (bitValue) {
                _block |= maskbit();
            } else {
                _block &= ~maskbit();
            }
            return *this;
        }

        BitReference& operator=(const BitReference& bit) noexcept {
            if (bit) {
                _block |= maskbit();
            } else {
                _block &= ~maskbit();
            }
            return *this;
        }

        /**
         * Return negated value of this bit.
         */
        bool operator~() const noexcept {
            return (_block & maskbit()) == 0;
        }

        /**
         * Return value of this bit.
         */
        operator bool() const noexcept {
            return (_block & maskbit()) != 0;
        }

        /**
         * Flip this bit.
         */
        BitReference& flip() noexcept {
            _block ^= maskbit();
            return *this;
        }

    private:
        inline BlockType maskbit() const noexcept {
            return bitset_utils::maskbit<BlockType>(_bitIndex);
        }

        BlockType& _block;
        size_t _bitIndex;
    };

    /**
     * Allocates a bitset of default size. The bitset of default size occupies all available inlined
     * storage and never allocates. The default size is equal to NumberOfBlocks * sizeof(BlockType)
     * * CHAR_BIT. E.g., DynamicBitset<uint8_t, 1> has default size 8 bits, DynamicBitset<uint64_t,
     * 2> - 128 bits.
     */
    DynamicBitset() : _storage(NumberOfBlocks) {}

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
        for (size_t i = 0; i < _storage.size(); ++i) {
            if (_storage[i] != kZero) {
                return true;
            }
        }
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
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] &= other._storage[i];
        }
        return *this;
    }

    /**
     * Compute OR of this subset and subset 'other' and assign the result to this subset. The
     * bitsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator|=(const DynamicBitset& other) {
        assertSize(other);
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] |= other._storage[i];
        }
        return *this;
    }

    /**
     * Compute XOR of this subset and subset 'other' and assign the result to this subset. The
     * bitsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator^=(const DynamicBitset& other) {
        assertSize(other);
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] ^= other._storage[i];
        }
        return *this;
    }

    /**
     * Compute a set difference of this subset and subset 'other' and assign the result to this
     * bitsets. The subsets must have the same size.
     */
    MONGO_COMPILER_ALWAYS_INLINE DynamicBitset& operator-=(const DynamicBitset& other) {
        assertSize(other);
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] &= ~other._storage[i];
        }
        return *this;
    }

    /**
     * Flip all bits of this subset.
     */
    MONGO_COMPILER_ALWAYS_INLINE void flip() {
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] = ~_storage[i];
        }
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
        for (size_t i = 0; i < _storage.size(); ++i) {
            _storage[i] = kOnes;
        }
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
        for (size_t i = 0; i < _storage.size(); ++i) {
            count += std::popcount(_storage[i]);
        }
        return count;
    }

    /**
     * Return true if this bitset is a subset of subset 'other'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool isSubsetOf(const DynamicBitset& other) const {
        return allOf([](BlockType thisBlock,
                        BlockType otherBlock) { return thisBlock == (thisBlock & otherBlock); },
                     _storage,
                     other._storage);
    }

    /**
     * Return true if this bitset has common set bits with 'other'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool intersects(const DynamicBitset& other) const {
        return anyOf(
            [](BlockType thisBlock, BlockType otherBlock) { return thisBlock & otherBlock; },
            _storage,
            other._storage);
    }

    /**
     * Return true if this bitset is equal to masked 'other'. This operation is
     * implementation of 'this == (mask & other)'.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool isEqualToMasked(const DynamicBitset& other,
                                                      const DynamicBitset& mask) const {
        return allOf([](BlockType thisBlock,
                        BlockType otherBlock,
                        BlockType maskBlock) { return thisBlock == (otherBlock & maskBlock); },
                     _storage,
                     other._storage,
                     mask._storage);
    }

    /**
     * Return the index of first set bit, if nothing found return 'kNpos'.
     */
    size_t findFirst() const {
        for (size_t i = 0; i < _storage.size(); ++i) {
            const auto block = _storage[i];
            if (block != kZero) {
                return i * Storage::kBlockSize + std::countr_zero(block);
            }
        }

        return kNpos;
    }

    /**
     * Return the index of the first set bit starting AFTER position 'previous', if nothing found
     * return 'kNpos'.
     */
    size_t findNext(size_t previous) const {
        const size_t size = this->size();
        if (++previous >= size) {
            return kNpos;
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
                return i * Storage::kBlockSize + std::countr_zero(block);
            }
        }

        return kNpos;
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
        return _storage.size() * Storage::kBlockSize;
    }

    /**
     * Iterates over integer blocks of the bitsets and returns true if the predicate returns true
     * for at least one set of corresponing blocks.
     */
    template <class Predicate, typename... Bitset>
    friend bool anyOf(Predicate predicate, const DynamicBitset& s, const Bitset&... ss) {
        return anyOf(predicate, s._storage, ss._storage...);
    }

    /**
     * Iterates over integer blocks of the bitsets and returns true if the predicate returns true
     * for all sets of corresponing blocks.
     */
    template <class Predicate, typename... Bitset>
    friend bool allOf(Predicate predicate, const DynamicBitset& s, const Bitset&... ss) {
        return allOf(predicate, s._storage, ss._storage...);
    }

    template <typename BT, size_t NB>
    friend bool operator==(const DynamicBitset<BT, NB>& lhs, const DynamicBitset<BT, NB>& rhs);

    template <typename BT, size_t NB>
    friend bool operator<(const DynamicBitset<BT, NB>& lhs, const DynamicBitset<BT, NB>& rhs);

    template <typename H>
    friend H AbslHashValue(H h, const DynamicBitset& bitset) {
        return H::combine(std::move(h), bitset._storage);
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE static size_t getRequiredNumberOfBlocks(
        size_t sizeInBits) noexcept {
        return (sizeInBits + Storage::kBlockSize - 1) / Storage::kBlockSize;
    }

    MONGO_COMPILER_ALWAYS_INLINE static size_t getBlockIndex(size_t index) noexcept {
        return index / Storage::kBlockSize;
    }

    MONGO_COMPILER_ALWAYS_INLINE static BlockType maskbit(size_t bitIndex) noexcept {
        return bitset_utils::maskbit<BlockType>(bitIndex);
    }

    /**
     * Returns bit inside inside its block.
     */
    MONGO_COMPILER_ALWAYS_INLINE static size_t getBitIndex(size_t index) noexcept {
        return index % Storage::kBlockSize;
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

/**
 * Return true if the given bitset are equal. The bitsets must be of the same size.
 */
template <typename T, size_t NumberOfBlocks>
bool operator==(const DynamicBitset<T, NumberOfBlocks>& lhs,
                const DynamicBitset<T, NumberOfBlocks>& rhs) {
    return lhs._storage == rhs._storage;
}

/*
 * Return true if 'lhs' bitset is lexicographically less than 'rhs', and false otherwise.
 */
template <typename T, size_t NumberOfBlocks>
bool operator<(const DynamicBitset<T, NumberOfBlocks>& lhs,
               const DynamicBitset<T, NumberOfBlocks>& rhs) {
    return lhs._storage < rhs._storage;
}

/**
 * Return a new bitset which is a result of AND operator of the given bitsets. The bitsets must be
 * of the same size.
 */
template <typename T, size_t NumberOfBlocks>
DynamicBitset<T, NumberOfBlocks> operator&(const DynamicBitset<T, NumberOfBlocks>& lhs,
                                           const DynamicBitset<T, NumberOfBlocks>& rhs) {
    DynamicBitset<T, NumberOfBlocks> result{lhs};
    result &= rhs;
    return result;
}

/**
 * Return a new bitset which is a result of OR operator of the given bitsets. The bitsets must be of
 * the same size.
 */
template <typename T, size_t NumberOfBlocks>
DynamicBitset<T, NumberOfBlocks> operator|(const DynamicBitset<T, NumberOfBlocks>& lhs,
                                           const DynamicBitset<T, NumberOfBlocks>& rhs) {
    DynamicBitset<T, NumberOfBlocks> result{lhs};
    result |= rhs;
    return result;
}

/**
 * Return a new bitset which is a result of XOR operator of the given bitsets. The bitsets must be
 * of the same size.
 */
template <typename T, size_t NumberOfBlocks>
DynamicBitset<T, NumberOfBlocks> operator^(const DynamicBitset<T, NumberOfBlocks>& lhs,
                                           const DynamicBitset<T, NumberOfBlocks>& rhs) {
    DynamicBitset<T, NumberOfBlocks> result{lhs};
    result ^= rhs;
    return result;
}

/**
 * Return a set difference of the given bitsets. The bitsets must be of the same size.
 */
template <typename T, size_t NumberOfBlocks>
DynamicBitset<T, NumberOfBlocks> operator-(const DynamicBitset<T, NumberOfBlocks>& lhs,
                                           const DynamicBitset<T, NumberOfBlocks>& rhs) {
    DynamicBitset<T, NumberOfBlocks> result{lhs};
    result -= rhs;
    return result;
}

template <typename T, size_t NumberOfBlocks>
std::ostream& operator<<(std::ostream& os, const DynamicBitset<T, NumberOfBlocks>& bitset) {
    for (size_t i = bitset.size(); i > 0; --i) {
        os << (bitset[i - 1] ? '1' : '0');
    }
    return os;
}
}  // namespace mongo
