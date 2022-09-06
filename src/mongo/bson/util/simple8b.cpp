/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/simple8b.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/platform/bits.h"

#include <algorithm>
#include <array>

namespace mongo {

namespace {
/*
 * Simple8B is a compression method for storing unsigned int 64 values. In this case
 * we make a few optimizations detailed below. We reserve the 4 lsbs for a baseSelector value. And
 * then we encode integers based on the following selector choice:
 *
 * Selector value:     0 |  1  2  3  4  5  6  7  8  9 10 11 12 13 14 | 15 (RLE)
 * Integers coded:     0 | 60 30 20 15 12 10  8  7  6  5  4  3  2  1 | up to 1920
 * Value Bits/integer: 0 |  1  2  3  4  5  6  7  8 10 12 15 20 30 60 | Last Value added
 * Wasted bits:        0 |  0  0  0  0  0  0  4  4  0  0  0  0  0  0 | 56
 * Total Bits/Integer: 0 |  1  2  3  4  5  6  7  8 10 12 15 20 30 60 | Last Valued added
 *
 * However, we make optimizations for selector value 7 and 8. We can see there are 4
 * wasted trailing bits. Using these 4 bits we can consider compression of trailing zeros.
 * For a selector extension value of 7, we store 4 bits and these represent up to 15 trailing zeros.
 * The extension bits are stored directly after the initial selector bits so that the simple8b word
 * looks like: | Base Selector (0-3) | Selector Extension (4-7) | Bits for Values (8 - 63)
 *
 * Selector Value:              0 | 7  7  7  7  7  7  7  7  7
 * Selector 7 Extension Value:  0 | 1  2  3  4  5  6  7  8  9
 * Value Bits/Integer:          0 | 2  3  4  5  7 10 14 24 52
 * TrailingZeroBits:            0 | 4  4  4  4  4  4  4  4  4
 * MaxTrailingZeroSize:         0 |15 15 15 15 15 15 15 15 15
 * Total Bits/Integer:          0 | 6  7  8  9 11 14 18 28 56
 *
 * Additionally, we consider larger trailing zero counts in selector 8. In this case the value
 * of the trailing zero bits is multiplied by a nibble shift of 4. We consider trailing zero sizes
 * of both 4 and 5 bits and thus, we split selector 8 in our implementation into Selector8Small and
 * Selector8Large
 *
 * Selector Value:             0 | 8   8   8   8   8   8   8   8   8   8   8   8   8
 * Selector 8 Extension Value: 0 | 1   2   3   4   5   6   7   8   9  10  11  12  13
 * Value Bits/Integer:         0 | 4   5   7  10  14  24  52   4   6   9  13  23  51
 * TrailingZeroBits:           0 | 4   4   4   4   4   4   4   5   5   5   5   5   5
 * MaxTrailingZerosSize:       0 |60  60  60  60  60  60  60 124 124 124 124 124 124
 * Total Bits/Integer:         0 | 8   9  11  14  18  28  56   9  11  14  18  28  56
 *
 * The simple8b words are according to this spec of selectors and their extension types.
 */

// Map selectorNames to their indexs.
static constexpr uint8_t kBaseSelector = 0;
static constexpr uint8_t kSevenSelector = 1;
static constexpr uint8_t kEightSelectorSmall = 2;
static constexpr uint8_t kEightSelectorLarge = 3;

// Variables to handle RLE
static constexpr uint8_t kRleSelector = 15;
static constexpr uint8_t kMaxRleCount = 16;
static constexpr uint8_t kRleMultiplier = 120;

// Mask to obtain the base and extended selectors.
static constexpr uint64_t kBaseSelectorMask = 0x000000000000000F;

// Selectors are always of size 4
static constexpr uint8_t kSelectorBits = 4;

// Nibble Shift is always of size 4
static constexpr uint8_t kNibbleShiftSize = 4;

// The max selector value for each extension
constexpr std::array<uint8_t, 4> kMaxSelector = {14, 9, 7, 13};

// The min selector value for each extension
constexpr std::array<uint8_t, 4> kMinSelector = {1, 1, 1, 8};

// The max amount of data bits each selector type can store. This is the amount of bits in the 64bit
// word that are not used for selector values.
constexpr std::array<uint8_t, 4> kDataBits = {60, 56, 56, 56};

// The amount of bits allocated to store a set of trailing zeros
constexpr std::array<uint8_t, 4> kTrailingZeroBitSize = {0, 4, 4, 5};

// The amount of possible trailing zeros each selector can handle in the trailingZeroBitSize
constexpr std::array<uint8_t, 4> kTrailingZerosMaxCount = {0, 15, 60, 124};

// Obtain a mask for the trailing zeros for the seven and eight selectors. We shift 4 and 5 bits to
// create the mask The trailingZeroBitSize variable is used as an index, but must be shifted - 4 to
// correspond to indexes 0 and 1.
constexpr std::array<uint8_t, 4> kTrailingZerosMask = {
    0, (1ull << 4) - 1, (1ull << 4) - 1, (1ull << 5) - 1};

// The amount of zeros each value in the trailing zero count represents
constexpr std::array<uint8_t, 4> kTrailingZerosMultiplier = {
    0, 1, kNibbleShiftSize, kNibbleShiftSize};

// Transfer from the base selector to the shift size.
constexpr std::array<uint8_t, 15> kBaseSelectorToShiftSize = {
    0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 0, 0, 0};

// Transfer from a selector to a specific extension type
// This is for selector 7 and 8 extensions where the selector value is passed along with
// selector index.
constexpr std::array<std::array<uint8_t, 14>, 2> kSelectorToExtension = {
    std::array<uint8_t, 14>{0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    std::array<uint8_t, 14>{0, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3}};

// Transfer from a extensionType and selectorIdx to the selector value to be held in the 4 lsb (base
// selector)
constexpr std::array<std::array<uint8_t, 16>, 4> kExtensionToBaseSelector = {
    std::array<uint8_t, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    std::array<uint8_t, 16>{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7},
    std::array<uint8_t, 16>{8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8},
    std::array<uint8_t, 16>{8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8}};


// A mask to obtain the value bits for each selector including the trailing zero bits. The masks are
// calculated as the following: Mask = 2^(kBitsPerInteger+kTrailingZeroBitSize) - 1
constexpr std::array<std::array<uint64_t, 16>, 4> kDecodeMask = {
    std::array<uint64_t, 16>{0,
                             1,
                             (1ull << 2) - 1,
                             (1ull << 3) - 1,
                             (1ull << 4) - 1,
                             (1ull << 5) - 1,
                             (1ull << 6) - 1,
                             (1ull << 7) - 1,
                             (1ull << 8) - 1,
                             (1ull << 10) - 1,
                             (1ull << 12) - 1,
                             (1ull << 15) - 1,
                             (1ull << 20) - 1,
                             (1ull << 30) - 1,
                             (1ull << 60) - 1,
                             1},
    std::array<uint64_t, 16>{0,
                             (1ull << 6) - 1,
                             (1ull << 7) - 1,
                             (1ull << 8) - 1,
                             (1ull << 9) - 1,
                             (1ull << 11) - 1,
                             (1ull << 14) - 1,
                             (1ull << 18) - 1,
                             (1ull << 28) - 1,
                             (1ull << 56) - 1,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0},
    std::array<uint64_t, 16>{0,
                             (1ull << 8) - 1,
                             (1ull << 9) - 1,
                             (1ull << 11) - 1,
                             (1ull << 14) - 1,
                             (1ull << 18) - 1,
                             (1ull << 28) - 1,
                             (1ull << 56) - 1,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0},
    std::array<uint64_t, 16>{
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        (1ull << 9) - 1,
        (1ull << 11) - 1,
        (1ull << 14) - 1,
        (1ull << 18) - 1,
        (1ull << 28) - 1,
        (1ull << 56) - 1,
        0,
        0}};

// The number of meaningful bits for each selector. This does not include any trailing zero bits.
// We use 64 bits for all invalid selectors, this is to make sure iteration does not get stuck.
constexpr std::array<std::array<uint8_t, 16>, 4> kBitsPerIntForSelector = {
    std::array<uint8_t, 16>{64, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60, 64},
    std::array<uint8_t, 16>{64, 2, 3, 4, 5, 7, 10, 14, 24, 52, 64, 64, 64, 64, 64, 64},
    std::array<uint8_t, 16>{64, 4, 5, 7, 10, 14, 24, 52, 0, 0, 64, 64, 64, 64, 64, 64},
    std::array<uint8_t, 16>{64, 0, 0, 0, 0, 0, 0, 0, 4, 6, 9, 13, 23, 51, 64, 64}};

// The number of integers coded for each selector.
constexpr std::array<std::array<uint8_t, 16>, 4> kIntsStoreForSelector = {
    std::array<uint8_t, 16>{0, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1, 0},
    std::array<uint8_t, 16>{0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0},
    std::array<uint8_t, 16>{0, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    std::array<uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 6, 5, 4, 3, 2, 1, 0, 0}};

// Calculates number of bits needed to store value. Must be less than
// numeric_limits<uint64_t>::max().
uint8_t _countBitsWithoutLeadingZeros(uint64_t value) {
    // All 1s is reserved for skip encoding so we add 1 to value to account for that case.
    return 64 - countLeadingZerosNonZero64(value + 1);
}

uint8_t _countTrailingZerosWithZero(uint64_t value) {
    // countTrailingZeros64 returns 64 if the value is 0 but we consider this to be 0 trailing
    // zeros.
    return value == 0 ? 0 : countTrailingZerosNonZero64(value);
}

uint8_t _countTrailingZerosWithZero(uint128_t value) {
    uint64_t low = absl::Uint128Low64(value);
    uint64_t high = absl::Uint128High64(value);

    // If value == 0 then we cannot add 64
    if (low == 0 && high != 0) {
        return countTrailingZerosNonZero64(high) + 64;
    } else {
        return _countTrailingZerosWithZero(low);
    }
}

// Calculates number of bits needed to store value. Must be less than
// numeric_limits<uint128_t>::max().
uint8_t _countBitsWithoutLeadingZeros(uint128_t value) {
    uint64_t high = absl::Uint128High64(value);
    if (high == 0) {
        uint64_t low = absl::Uint128Low64(value);
        // We can't call _countBitsWithoutLeadingZeros() with numeric_limits<uint64_t>::max as it
        // would overflow and yield the wrong result. Just return the correct value instead.
        if (low == std::numeric_limits<uint64_t>::max())
            return 65;
        return _countBitsWithoutLeadingZeros(low);
    } else {
        return 128 - countLeadingZerosNonZero64(high);
    }
}

/*
 * This method takes a number of intsNeeded and an extensionType and returns the selector index for
 * that type. This method should never fail as it is called when we are encoding a largest value.
 */
uint8_t _getSelectorIndex(uint8_t intsNeeded, uint8_t extensionType) {
    auto iteratorIdx = std::find_if(
        kIntsStoreForSelector[extensionType].begin() + kMinSelector[extensionType],
        kIntsStoreForSelector[extensionType].begin() + kMaxSelector[extensionType],
        [intsNeeded](uint8_t intsPerSelectorIdx) { return intsNeeded >= intsPerSelectorIdx; });
    return iteratorIdx - kIntsStoreForSelector[extensionType].begin();
}

}  // namespace

// This is called in _encode while iterating through _pendingValues. For the base selector, we just
// return val. Contains unsed vars in order to seamlessly integrate with seven and eight selector
// extensions.
template <typename T>
struct Simple8bBuilder<T>::BaseSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        return static_cast<uint64_t>(value.value());
    };
};

// This is called in _encode while iterating through _pendingValues. It creates part of a simple8b
// word according to the specifications of the sevenSelector extension. This value is then appended
// to the full simple8b word in _encode.
template <typename T>
struct Simple8bBuilder<T>::SevenSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        uint8_t trailingZeros = value.trailingZerosCount[kSevenSelector];
        uint64_t currWord = trailingZeros;
        // We do two shifts here to account for the case where trailingZeros is > kTrailingZero bit
        // size. If we subtracted this could lead to shift by a negative value which is undefined.
        currWord |= static_cast<uint64_t>((value.value() >> trailingZeros)
                                          << kTrailingZeroBitSize[kSevenSelector]);
        return currWord;
    };
};

// This is a helper functor that is extended by the EightSelectorSmall and EightSelectorLarge encode
// functors. It provides the logic for encoding with the eight selector where the extension type is
// designated by the inheritance in the EightSelectorSmall and EightSelectorLarge functors.
template <typename T>
template <uint8_t ExtensionType>
struct Simple8bBuilder<T>::EightSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        // integer division. We have a nibble shift of size 4
        uint8_t trailingZeros = value.trailingZerosCount[ExtensionType] / kNibbleShiftSize;
        uint64_t currWord = trailingZeros;
        // Shift to remove trailing zeros * 4 and then shift over for the 4 bits to hold
        // the trailingZerosCount
        currWord |= static_cast<uint64_t>((value.value() >> (trailingZeros * kNibbleShiftSize))
                                          << kTrailingZeroBitSize[ExtensionType]);
        return currWord;
    }
};

// This is called in _encode while iterating through _pendingValues. It creates part of a simple8b
// word according to the specifications of the eightSelectorSmall extension. This value is then
// appended to the full simple8b word in _encode.
template <typename T>
struct Simple8bBuilder<T>::EightSelectorSmallEncodeFunctor
    : public EightSelectorEncodeFunctor<kEightSelectorSmall> {};

// This is called in _encode while iterating through _pendingValues. It creates part of a simple8b
// word according to the specifications of the eightSelectorLarge extension. This value is then
// appended to the full simple8b word in _encode.
template <typename T>
struct Simple8bBuilder<T>::EightSelectorLargeEncodeFunctor
    : public EightSelectorEncodeFunctor<kEightSelectorLarge> {};

// Base Constructor for PendingValue
template <typename T>
Simple8bBuilder<T>::PendingValue::PendingValue(
    boost::optional<T> val,
    std::array<uint8_t, kNumOfSelectorTypes> bitCount,
    std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount)
    : val(val), bitCount(bitCount), trailingZerosCount(trailingZerosCount){};

template <typename T>
Simple8bBuilder<T>::PendingIterator::PendingIterator(
    typename std::deque<PendingValue>::const_iterator beginning,
    typename std::deque<PendingValue>::const_iterator it,
    reference rleValue,
    uint32_t rleCount)
    : _begin(beginning), _it(it), _rleValue(rleValue), _rleCount(rleCount) {}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator-> () const -> pointer {
    return &operator*();
}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator*() const -> reference {
    if (_rleCount > 0)
        return _rleValue;

    return _it->val;
}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator++() -> PendingIterator& {
    if (_rleCount > 0) {
        --_rleCount;
        return *this;
    }

    ++_it;
    return *this;
}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator++(int) -> PendingIterator {
    auto ret = *this;
    ++(*this);
    return ret;
}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator--() -> PendingIterator& {
    if (_rleCount > 0 || _it == _begin) {
        ++_rleCount;
        return *this;
    }

    --_it;
    return *this;
}

template <typename T>
auto Simple8bBuilder<T>::PendingIterator::operator--(int) -> PendingIterator {
    auto ret = *this;
    --(*this);
    return ret;
}

template <typename T>
bool Simple8bBuilder<T>::PendingIterator::operator==(
    const Simple8bBuilder<T>::PendingIterator& rhs) const {
    return _it == rhs._it && _rleCount == rhs._rleCount;
}

template <typename T>
bool Simple8bBuilder<T>::PendingIterator::operator!=(
    const Simple8bBuilder<T>::PendingIterator& rhs) const {
    return !operator==(rhs);
}

template <typename T>
Simple8bBuilder<T>::Simple8bBuilder(Simple8bWriteFn writeFunc) : _writeFn(std::move(writeFunc)) {}

template <typename T>
Simple8bBuilder<T>::~Simple8bBuilder() = default;

template <typename T>
bool Simple8bBuilder<T>::append(T value) {
    if (_rlePossible()) {
        if (_lastValueInPrevWord.val == value) {
            ++_rleCount;
            return true;
        }
        _handleRleTermination();
    }

    return _appendValue(value, true);
}

template <typename T>
void Simple8bBuilder<T>::skip() {
    if (_rlePossible() && _lastValueInPrevWord.isSkip()) {
        ++_rleCount;
        return;
    }

    _handleRleTermination();
    _appendSkip(true /* tryRle */);
}

template <typename T>
void Simple8bBuilder<T>::flush() {
    // Flush repeating integers that have been kept for RLE.
    _handleRleTermination();
    // Flush buffered values in _pendingValues.
    if (!_pendingValues.empty()) {
        // always flush with the most recent valid selector. This value is the baseSelector if we
        // have not have a valid selector yet.
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            _writeFn(simple8bWord);
        } while (!_pendingValues.empty());

        // There are no more words in _pendingValues and RLE is possible.
        // However the _rleCount is 0 because we have not read any of the values in the next word.
        _rleCount = 0;
    }

    // Always reset _lastValueInPrevWord. We may only start RLE after flush on 0 value.
    _lastValueInPrevWord = {};
}

template <typename T>
bool Simple8bBuilder<T>::_appendValue(T value, bool tryRle) {
    // Early exit if we try to store max value. They are not handled when counting zeros.
    if (value == std::numeric_limits<T>::max())
        return false;

    uint8_t trailingZerosCount = _countTrailingZerosWithZero(value);
    // Initially set every selector as invalid.
    uint8_t bitCountWithoutLeadingZeros = _countBitsWithoutLeadingZeros(value);
    uint8_t trailingZerosStoredInCountSeven =
        (std::min(trailingZerosCount, kTrailingZerosMaxCount[kSevenSelector]));
    uint8_t meaningfulValueBitsStoredWithSeven =
        bitCountWithoutLeadingZeros - trailingZerosStoredInCountSeven;
    // We use integer division to ensure that a multiple of 4 is stored in
    // trailingZerosStoredInCount when we have the nibble shift.
    uint8_t trailingZerosStoredInCountEightSmall =
        (std::min(trailingZerosCount, kTrailingZerosMaxCount[kEightSelectorSmall]) /
         kNibbleShiftSize) *
        kNibbleShiftSize;
    uint8_t meaningfulValueBitsStoredWithEightSmall =
        bitCountWithoutLeadingZeros - trailingZerosStoredInCountEightSmall;
    // We use integer division to ensure that a multiple of 4 is stored in
    // trailingZerosStoredInCount when we have the nibble shift.
    uint8_t trailingZerosStoredInCountEightLarge =
        (std::min(trailingZerosCount, kTrailingZerosMaxCount[kEightSelectorLarge]) /
         kNibbleShiftSize) *
        kNibbleShiftSize;
    uint8_t meaningfulValueBitsStoredWithEightLarge =
        bitCountWithoutLeadingZeros - trailingZerosStoredInCountEightLarge;

    // Edge cases where we have the number of trailing zeros bits as all ones and we need to add a
    // padded zero to the meaningful bits to avoid confilicts with skip storage. Otherwise, we can
    // reuse the bitCountWithoutLeadingZeros already calculated above.
    if (trailingZerosCount == kTrailingZerosMaxCount[kSevenSelector]) {
        meaningfulValueBitsStoredWithSeven =
            _countBitsWithoutLeadingZeros(value >> trailingZerosCount);
    } else if (trailingZerosCount == kTrailingZerosMaxCount[kEightSelectorSmall]) {
        meaningfulValueBitsStoredWithEightSmall =
            _countBitsWithoutLeadingZeros(value >> trailingZerosCount);
    }

    // This case is specifically for 128 bit types where we have 124 zeros or max zeros
    // count. We do not need to even check this for 64 bit types
    if constexpr (std::is_same<T, uint128_t>::value) {
        if (trailingZerosCount == kTrailingZerosMaxCount[kEightSelectorLarge]) {
            meaningfulValueBitsStoredWithEightLarge =
                _countBitsWithoutLeadingZeros(value >> trailingZerosCount);
        }
    }

    std::array<uint8_t, 4> zeroCount = {0,
                                        trailingZerosStoredInCountSeven,
                                        trailingZerosStoredInCountEightSmall,
                                        trailingZerosStoredInCountEightLarge};

    // Check if the amount of bits needed is more than we can store using all selector combinations.
    if ((bitCountWithoutLeadingZeros > kDataBits[kBaseSelector]) &&
        (meaningfulValueBitsStoredWithSeven + kTrailingZeroBitSize[kSevenSelector] >
         kDataBits[kSevenSelector]) &&
        (meaningfulValueBitsStoredWithEightSmall + kTrailingZeroBitSize[kEightSelectorSmall] >
         kDataBits[kEightSelectorSmall]) &&
        (meaningfulValueBitsStoredWithEightLarge + kTrailingZeroBitSize[kEightSelectorLarge] >
         kDataBits[kEightSelectorLarge])) {
        return false;
    }

    PendingValue pendingValue(value,
                              {bitCountWithoutLeadingZeros,
                               meaningfulValueBitsStoredWithSeven,
                               meaningfulValueBitsStoredWithEightSmall,
                               meaningfulValueBitsStoredWithEightLarge},
                              zeroCount);
    // Check if we have a valid selector for the current word. This method update the global
    // isSelectorValid to avoid redundant computation.
    if (_doesIntegerFitInCurrentWord(pendingValue)) {
        // If the integer fits in the current word, add it.
        _pendingValues.push_back(pendingValue);
        _updateSimple8bCurrentState(pendingValue);
    } else {
        // If the integer does not fit in the current word, convert the integers into simple8b
        // word(s) with no unused buckets until the new value can be added to _pendingValues. Then
        // add the Simple8b word(s) to the buffer. Finally add the new integer and update any global
        // variables. We add based on the lastSelector that was valid where priority ordering is the
        // following: base, seven, eightSmall, eightLarge. Store pending last value for RLE.
        PendingValue lastPendingValue = _pendingValues.back();
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            _writeFn(simple8bWord);
        } while (!(_doesIntegerFitInCurrentWord(pendingValue)));

        if (tryRle && _pendingValues.empty() && lastPendingValue.val == value) {
            // There are no more words in _pendingValues and the last element of the last Simple8b
            // word is the same as the new value. Therefore, start RLE.
            _rleCount = 1;
            _lastValueInPrevWord = lastPendingValue;
        } else {
            _pendingValues.push_back(pendingValue);
            _updateSimple8bCurrentState(pendingValue);
        }
    }
    return true;
}

template <typename T>
void Simple8bBuilder<T>::_appendSkip(bool tryRle) {
    if (!_pendingValues.empty()) {
        bool isLastValueSkip = _pendingValues.back().isSkip();

        // There is never a case where we need to write more than one Simple8b wrod
        // because we only need 1 bit for skip
        if (!_doesIntegerFitInCurrentWord({boost::none, kMinDataBits, {0, 0, 0, 0}})) {
            // Form simple8b word if skip can not fit with last selector
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            _writeFn(simple8bWord);
            _lastValidExtensionType = kBaseSelector;
        }

        if (_pendingValues.empty() && isLastValueSkip && tryRle) {
            // It is possible to start rle
            _rleCount = 1;
            _lastValueInPrevWord = {boost::none, {0, 0, 0, 0}, {0, 0, 0, 0}};
            return;
        }
    }
    // Push true into skip and the dummy value, 0, into currNum. We use the dummy value, 0 because
    // it takes 1 bit and it will not affect our global curr bit length calculations.
    _pendingValues.push_back({boost::none, {0, 0, 0, 0}, {0, 0, 0, 0}});
}

template <typename T>
void Simple8bBuilder<T>::_handleRleTermination() {
    if (_rleCount == 0)
        return;

    // Try to create a RLE Simple8b word.
    _appendRleEncoding();
    // Add any values that could not be encoded in RLE.
    while (_rleCount > 0) {
        if (_lastValueInPrevWord.isSkip()) {
            _appendSkip(false /* tryRle */);
        } else {
            _appendValue(_lastValueInPrevWord.value(), false);
        }
        --_rleCount;
    }
}

template <typename T>
void Simple8bBuilder<T>::_appendRleEncoding() {
    // This encodes a value using rle. The selector is set as 15 and the count is added in the next
    // 4 bits. The value is the previous value stored by simple8b or 0 if no previous value was
    // stored.
    auto createRleEncoding = [this](uint8_t count) {
        uint64_t rleEncoding = kRleSelector;
        // We will store (count - 1) during encoding and execute (count + 1) during decoding.
        rleEncoding |= (count - 1) << kSelectorBits;
        _writeFn(rleEncoding);
    };

    uint32_t count = _rleCount / kRleMultiplier;
    // Check to make sure count is big enough for RLE encoding
    if (count >= 1) {
        while (count > kMaxRleCount) {
            // If one RLE word is insufficient use multiple RLE words.
            createRleEncoding(kMaxRleCount);
            count -= kMaxRleCount;
        }
        createRleEncoding(count);
        _rleCount %= kRleMultiplier;
    }
}

template <typename T>
bool Simple8bBuilder<T>::_rlePossible() const {
    return _pendingValues.empty() || _rleCount != 0;
}


template <typename T>
bool Simple8bBuilder<T>::_doesIntegerFitInCurrentWord(const PendingValue& value) {
    bool fitsInCurrentWord = false;
    for (uint8_t i = 0; i < kNumOfSelectorTypes; ++i) {
        if (isSelectorPossible[i]) {
            fitsInCurrentWord =
                fitsInCurrentWord || _doesIntegerFitInCurrentWordWithGivenSelectorType(value, i);
        }
        // Stop loop early if we find a valid selector.
        if (fitsInCurrentWord)
            return fitsInCurrentWord;
    }
    return false;
}

template <typename T>
bool Simple8bBuilder<T>::_doesIntegerFitInCurrentWordWithGivenSelectorType(
    const PendingValue& value, uint8_t extensionType) {
    uint64_t numBitsWithValue =
        (std::max(_currMaxBitLen[extensionType], value.bitCount[extensionType]) +
         kTrailingZeroBitSize[extensionType]) *
        (_pendingValues.size() + 1);
    // If the numBitswithValue is greater than max bits or we cannot fit the trailingZeros we update
    // this selector as false and return false. Special case for baseSelector where we never add
    // trailingZeros so we always pass the zeros comparison.
    if (kDataBits[extensionType] < numBitsWithValue) {
        isSelectorPossible[extensionType] = false;
        return false;
    }
    // Update so we remember the last validExtensionType when its time to encode a word
    _lastValidExtensionType = extensionType;
    return true;
}

template <typename T>
int64_t Simple8bBuilder<T>::_encodeLargestPossibleWord(uint8_t extensionType) {
    // Since this is always called right after _doesIntegerFitInCurrentWord fails for the first
    // time, we know all values in _pendingValues fits in the slots for the selector that can store
    // this many values. Find the smallest selector that doesn't leave any unused slots.
    uint8_t selector = _getSelectorIndex(_pendingValues.size(), extensionType);
    uint8_t integersCoded = kIntsStoreForSelector[extensionType][selector];
    uint64_t encodedWord;
    switch (extensionType) {
        case kEightSelectorSmall:
            encodedWord = _encode(EightSelectorSmallEncodeFunctor(), selector, extensionType);
            break;
        case kEightSelectorLarge:
            encodedWord = _encode(EightSelectorLargeEncodeFunctor(), selector, extensionType);
            break;
        case kSevenSelector:
            encodedWord = _encode(SevenSelectorEncodeFunctor(), selector, extensionType);
            break;
        default:
            encodedWord = _encode(BaseSelectorEncodeFunctor(), selector, extensionType);
    }

    _pendingValues.erase(_pendingValues.begin(), _pendingValues.begin() + integersCoded);
    _currMaxBitLen = kMinDataBits;
    for (const auto& val : _pendingValues) {
        _updateSimple8bCurrentState(val);
    }
    // Reset which selectors are possible to use for next word
    isSelectorPossible.fill(true);
    return encodedWord;
}

template <typename T>
template <typename Func>
uint64_t Simple8bBuilder<T>::_encode(Func func, uint8_t selectorIdx, uint8_t extensionType) {
    uint8_t baseSelector = kExtensionToBaseSelector[extensionType][selectorIdx];
    uint8_t bitShiftExtension = kBaseSelectorToShiftSize[baseSelector];
    uint64_t encodedWord = baseSelector;
    uint8_t bitsPerInteger = kBitsPerIntForSelector[extensionType][selectorIdx];
    uint8_t integersCoded = kIntsStoreForSelector[extensionType][selectorIdx];
    uint64_t unshiftedMask = kDecodeMask[extensionType][selectorIdx];
    uint8_t bitsForTrailingZeros = kTrailingZeroBitSize[extensionType];
    for (uint8_t i = 0; i < integersCoded; ++i) {
        uint8_t shiftSize =
            (bitsPerInteger + bitsForTrailingZeros) * i + kSelectorBits + bitShiftExtension;
        uint64_t currEncodedWord;
        if (_pendingValues[i].isSkip()) {
            currEncodedWord = unshiftedMask;
        } else {
            currEncodedWord = func(_pendingValues[i]);
        }
        encodedWord |= currEncodedWord << shiftSize;
    }
    if (extensionType != kBaseSelector) {
        encodedWord |= (uint64_t(selectorIdx) << kSelectorBits);
    }
    return encodedWord;
}

template <typename T>
void Simple8bBuilder<T>::_updateSimple8bCurrentState(const PendingValue& val) {
    for (uint8_t i = 0; i < kNumOfSelectorTypes; ++i) {
        _currMaxBitLen[i] = std::max(_currMaxBitLen[i], val.bitCount[i]);
    }
}

template <typename T>
typename Simple8bBuilder<T>::PendingIterator Simple8bBuilder<T>::begin() const {
    return {_pendingValues.begin(), _pendingValues.begin(), _lastValueInPrevWord.val, _rleCount};
}

template <typename T>
typename Simple8bBuilder<T>::PendingIterator Simple8bBuilder<T>::end() const {
    return {_pendingValues.begin(), _pendingValues.end(), _lastValueInPrevWord.val, 0};
}

template <typename T>
std::reverse_iterator<typename Simple8bBuilder<T>::PendingIterator> Simple8bBuilder<T>::rbegin()
    const {
    return std::reverse_iterator<typename Simple8bBuilder<T>::PendingIterator>(end());
}

template <typename T>
std::reverse_iterator<typename Simple8bBuilder<T>::PendingIterator> Simple8bBuilder<T>::rend()
    const {
    return std::reverse_iterator<typename Simple8bBuilder<T>::PendingIterator>(begin());
}

template <typename T>
void Simple8bBuilder<T>::setWriteCallback(Simple8bWriteFn writer) {
    _writeFn = std::move(writer);
}

template <typename T>
Simple8b<T>::Iterator::Iterator(const char* pos,
                                const char* end,
                                const boost::optional<T>& previous)
    : _pos(pos), _end(end), _value(previous), _rleRemaining(0), _shift(0) {
    if (pos != end) {
        _loadBlock();
    }
}

template <typename T>
void Simple8b<T>::Iterator::_loadBlock() {
    _current = ConstDataView(_pos).read<LittleEndian<uint64_t>>();

    _selector = _current & kBaseSelectorMask;
    uint8_t selectorExtension = ((_current >> kSelectorBits) & kBaseSelectorMask);

    // If RLE selector, just load remaining count. Keep value from previous.
    if (_selector == kRleSelector) {
        // Set shift to something larger than 64bit to force a new block to be loaded when
        // we've extinguished RLE count.
        _shift = (sizeof(_current) * 8) + 1;
        _rleRemaining = _rleCountInCurrent(selectorExtension) - 1;
        return;
    }

    _extensionType = kBaseSelector;
    uint8_t extensionBits = 0;

    // If Selectors 7 or 8 check if we are using extended selectors
    if (_selector == 7 || _selector == 8) {
        _extensionType = kSelectorToExtension[_selector - 7][selectorExtension];
        // Use the extended selector if extension is != 0
        if (_extensionType != kBaseSelector) {
            _selector = selectorExtension;
            // Make shift the size of 2 selectors to handle extensions
        }
        extensionBits = 4;
    }

    // Initialize all variables needed to advance the iterator for this block
    _mask = kDecodeMask[_extensionType][_selector];
    _countMask = kTrailingZerosMask[_extensionType];
    _countBits = kTrailingZeroBitSize[_extensionType];
    _countMultiplier = kTrailingZerosMultiplier[_extensionType];
    _bitsPerValue = kBitsPerIntForSelector[_extensionType][_selector] + _countBits;
    _shift = kSelectorBits + extensionBits;
    _rleRemaining = 0;

    // Finally load the first value in the block.
    _loadValue();
}

template <typename T>
void Simple8b<T>::Iterator::_loadValue() {
    // Mask out the value of current slot
    auto shiftedMask = _mask << _shift;
    uint64_t value = (_current & shiftedMask) >> _shift;

    // Check if this a skip
    if (value == _mask) {
        _value = boost::none;
        return;
    }

    // Shift in any trailing zeros that are stored in the count for extended selectors 7 and 8.
    auto trailingZeros = (value & _countMask);
    _value = static_cast<T>((value >> _countBits)) << (trailingZeros * _countMultiplier);
}

template <typename T>
size_t Simple8b<T>::Iterator::blockSize() const {
    if (_selector == kRleSelector) {
        uint8_t selectorExtension = (_current >> kSelectorBits) & kBaseSelectorMask;
        return _rleCountInCurrent(selectorExtension);
    }
    return kIntsStoreForSelector[_extensionType][_selector];
}

template <typename T>
uint16_t Simple8b<T>::Iterator::_rleCountInCurrent(uint8_t selectorExtension) const {
    // SelectorExtension holds the rle count in this case
    return (selectorExtension + 1) * kRleMultiplier;
}

template <typename T>
typename Simple8b<T>::Iterator& Simple8b<T>::Iterator::operator++() {
    if (_rleRemaining > 0) {
        --_rleRemaining;
        return *this;
    }

    _shift += _bitsPerValue;
    if (_shift + _bitsPerValue > sizeof(_current) * 8) {
        return advanceBlock();
    }

    _loadValue();
    return *this;
}

template <typename T>
typename Simple8b<T>::Iterator& Simple8b<T>::Iterator::advanceBlock() {
    _pos += sizeof(uint64_t);
    if (_pos == _end) {
        _rleRemaining = 0;
        _shift = 0;
        return *this;
    }

    _loadBlock();
    return *this;
}

template <typename T>
bool Simple8b<T>::Iterator::operator==(const Simple8b::Iterator& rhs) const {
    return _pos == rhs._pos && _rleRemaining == rhs._rleRemaining && _shift == rhs._shift;
}

template <typename T>
bool Simple8b<T>::Iterator::operator!=(const Simple8b::Iterator& rhs) const {
    return !operator==(rhs);
}

template <typename T>
Simple8b<T>::Simple8b(const char* buffer, int size, boost::optional<T> previous)
    : _buffer(buffer), _size(size), _previous(previous) {
    invariant(size % sizeof(uint64_t) == 0);
}

template <typename T>
typename Simple8b<T>::Iterator Simple8b<T>::begin() const {
    return {_buffer, _buffer + _size, _previous};
}

template <typename T>
typename Simple8b<T>::Iterator Simple8b<T>::end() const {
    return {_buffer + _size, _buffer + _size, boost::none};
}

template class Simple8b<uint64_t>;
template class Simple8b<uint128_t>;
template class Simple8bBuilder<uint64_t>;
template class Simple8bBuilder<uint128_t>;
}  // namespace mongo
