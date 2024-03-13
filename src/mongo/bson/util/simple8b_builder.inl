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

#include "mongo/bson/util/simple8b_builder.h"

#include <algorithm>
#include <array>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/util/simple8b_helpers.h"

namespace mongo {
// This is called in _encode while iterating through _pendingValues. For the base selector, we just
// return val. Contains unsed vars in order to seamlessly integrate with seven and eight selector
// extensions.
template <typename T, class Allocator>
struct Simple8bBuilder<T, Allocator>::BaseSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        return static_cast<uint64_t>(value.value());
    };
};

// This is called in _encode while iterating through _pendingValues. It creates part of a simple8b
// word according to the specifications of the sevenSelector extension. This value is then appended
// to the full simple8b word in _encode.
template <typename T, class Allocator>
struct Simple8bBuilder<T, Allocator>::SevenSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        using namespace simple8b_internal;

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
template <typename T, class Allocator>
template <uint8_t ExtensionType>
struct Simple8bBuilder<T, Allocator>::EightSelectorEncodeFunctor {
    uint64_t operator()(const PendingValue& value) {
        using namespace simple8b_internal;

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
template <typename T, class Allocator>
struct Simple8bBuilder<T, Allocator>::EightSelectorSmallEncodeFunctor
    : public EightSelectorEncodeFunctor<simple8b_internal::kEightSelectorSmall> {};

// This is called in _encode while iterating through _pendingValues. It creates part of a simple8b
// word according to the specifications of the eightSelectorLarge extension. This value is then
// appended to the full simple8b word in _encode.
template <typename T, class Allocator>
struct Simple8bBuilder<T, Allocator>::EightSelectorLargeEncodeFunctor
    : public EightSelectorEncodeFunctor<simple8b_internal::kEightSelectorLarge> {};

// Base Constructor for PendingValue
template <typename T, class Allocator>
Simple8bBuilder<T, Allocator>::PendingValue::PendingValue(
    boost::optional<T> val,
    std::array<uint8_t, kNumOfSelectorTypes> bitCount,
    std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount)
    : val(val), bitCount(bitCount), trailingZerosCount(trailingZerosCount){};

template <typename T, class Allocator>
Simple8bBuilder<T, Allocator>::PendingIterator::PendingIterator(
    typename std::vector<PendingValue, PendingValueAllocator>::const_iterator beginning,
    typename std::vector<PendingValue, PendingValueAllocator>::const_iterator it,
    reference rleValue,
    uint32_t rleCount)
    : _begin(beginning), _it(it), _rleValue(rleValue), _rleCount(rleCount) {}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator->() const -> pointer {
    return &operator*();
}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator*() const -> reference {
    if (_rleCount > 0)
        return _rleValue;

    return _it->val;
}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator++() -> PendingIterator& {
    if (_rleCount > 0) {
        --_rleCount;
        return *this;
    }

    ++_it;
    return *this;
}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator++(int) -> PendingIterator {
    auto ret = *this;
    ++(*this);
    return ret;
}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator--() -> PendingIterator& {
    if (_rleCount > 0 || _it == _begin) {
        ++_rleCount;
        return *this;
    }

    --_it;
    return *this;
}

template <typename T, class Allocator>
auto Simple8bBuilder<T, Allocator>::PendingIterator::operator--(int) -> PendingIterator {
    auto ret = *this;
    --(*this);
    return ret;
}

template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::PendingIterator::operator==(
    const Simple8bBuilder<T, Allocator>::PendingIterator& rhs) const {
    return _it == rhs._it && _rleCount == rhs._rleCount;
}

template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::PendingIterator::operator!=(
    const Simple8bBuilder<T, Allocator>::PendingIterator& rhs) const {
    return !operator==(rhs);
}

template <typename T, class Allocator>
Simple8bBuilder<T, Allocator>::Simple8bBuilder(Allocator allocator) : _pendingValues(allocator) {}

template <typename T, class Allocator>
Simple8bBuilder<T, Allocator>::~Simple8bBuilder() = default;

template <typename T, class Allocator>
template <class F>
requires Simple8bBlockWriter<F>
bool Simple8bBuilder<T, Allocator>::append(T value, F&& writeFn) {
    if (_rlePossible()) {
        if (_lastValueInPrevWord == value) {
            ++_rleCount;
            return true;
        }
        _handleRleTermination(writeFn);
    }

    return _appendValue(value, true, writeFn);
}

template <typename T, class Allocator>
template <class F>
requires Simple8bBlockWriter<F>
void Simple8bBuilder<T, Allocator>::skip(F&& writeFn) {
    if (_rlePossible() && !_lastValueInPrevWord.has_value()) {
        ++_rleCount;
        return;
    }

    _handleRleTermination(writeFn);
    _appendSkip(true /* tryRle */, writeFn);
}

template <typename T, class Allocator>
template <class F>
requires Simple8bBlockWriter<F>
void Simple8bBuilder<T, Allocator>::flush(F&& writeFn) {
    // Flush repeating integers that have been kept for RLE.
    _handleRleTermination(writeFn);
    // Flush buffered values in _pendingValues.
    if (!_pendingValues.empty()) {
        // always flush with the most recent valid selector. This value is the baseSelector if we
        // have not have a valid selector yet.
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            writeFn(simple8bWord);
        } while (!_pendingValues.empty());

        // There are no more words in _pendingValues and RLE is possible.
        // However the _rleCount is 0 because we have not read any of the values in the next word.
        _rleCount = 0;
    }

    // Always reset _lastValueInPrevWord. We may only start RLE after flush on 0 value.
    _lastValueInPrevWord = 0;
    _lastValidExtensionType = 0;
}

template <typename T, class Allocator>
void Simple8bBuilder<T, Allocator>::setLastForRLE(boost::optional<T> val) {
    _lastValueInPrevWord = val;
}

template <typename T, class Allocator>
void Simple8bBuilder<T, Allocator>::resetLastForRLEIfNeeded() {
    if (!_rlePossible()) {
        _lastValueInPrevWord = 0;
    }
}

template <typename T, class Allocator>
void Simple8bBuilder<T, Allocator>::initializeRLEFrom(const Simple8bBuilder<T, Allocator>& other) {
    if (other._rlePossible()) {
        _lastValueInPrevWord = other._lastValueInPrevWord;
    }
}

template <typename T, class Allocator>
boost::optional<typename Simple8bBuilder<T, Allocator>::PendingValue>
Simple8bBuilder<T, Allocator>::_calculatePendingValue(T value) {
    using namespace simple8b_internal;

    // Early exit if we try to store max value. They are not handled when counting zeros.
    if (value == std::numeric_limits<T>::max())
        return boost::none;

    uint8_t trailingZerosCount = countTrailingZerosWithZero(value);
    // Initially set every selector as invalid.
    uint8_t bitCountWithoutLeadingZeros = countBitsWithoutLeadingZeros(value);
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
            countBitsWithoutLeadingZeros(value >> trailingZerosCount);
    } else if (trailingZerosCount == kTrailingZerosMaxCount[kEightSelectorSmall]) {
        meaningfulValueBitsStoredWithEightSmall =
            countBitsWithoutLeadingZeros(value >> trailingZerosCount);
    }

    // This case is specifically for 128 bit types where we have 124 zeros or max zeros
    // count. We do not need to even check this for 64 bit types
    if constexpr (std::is_same<T, uint128_t>::value) {
        if (trailingZerosCount == kTrailingZerosMaxCount[kEightSelectorLarge]) {
            meaningfulValueBitsStoredWithEightLarge =
                countBitsWithoutLeadingZeros(value >> trailingZerosCount);
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
        return boost::none;
    }

    return PendingValue{value,
                        {bitCountWithoutLeadingZeros,
                         meaningfulValueBitsStoredWithSeven,
                         meaningfulValueBitsStoredWithEightSmall,
                         meaningfulValueBitsStoredWithEightLarge},
                        zeroCount};
}

template <typename T, class Allocator>
template <class F>
bool Simple8bBuilder<T, Allocator>::_appendValue(T value, bool tryRle, F&& writeFn) {
    auto pendingValue = _calculatePendingValue(value);
    if (!pendingValue) {
        return false;
    }

    // Check if we have a valid selector for the current word. This method update the global
    // isSelectorValid to avoid redundant computation.
    if (_doesIntegerFitInCurrentWord(*pendingValue)) {
        // If the integer fits in the current word, add it.
        _pendingValues.push_back(*pendingValue);
        _updateSimple8bCurrentState(*pendingValue);
    } else {
        // If the integer does not fit in the current word, convert the integers into simple8b
        // word(s) with no unused buckets until the new value can be added to _pendingValues. Then
        // add the Simple8b word(s) to the buffer. Finally add the new integer and update any global
        // variables. We add based on the lastSelector that was valid where priority ordering is the
        // following: base, seven, eightSmall, eightLarge. Store pending last value for RLE.
        PendingValue lastPendingValue = _pendingValues.back();
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            writeFn(simple8bWord);
        } while (!(_doesIntegerFitInCurrentWord(*pendingValue)));

        if (tryRle && _pendingValues.empty() && lastPendingValue.val == value) {
            // There are no more words in _pendingValues and the last element of the last Simple8b
            // word is the same as the new value. Therefore, start RLE.
            _rleCount = 1;
            _lastValueInPrevWord = lastPendingValue.val;
        } else {
            _pendingValues.push_back(*pendingValue);
            _updateSimple8bCurrentState(*pendingValue);
        }
    }
    return true;
}

template <typename T, class Allocator>
template <class F>
void Simple8bBuilder<T, Allocator>::_appendSkip(bool tryRle, F&& writeFn) {
    using namespace simple8b_internal;

    if (!_pendingValues.empty()) {
        bool isLastValueSkip = _pendingValues.back().isSkip();

        // There is never a case where we need to write more than one Simple8b wrod
        // because we only need 1 bit for skip
        if (!_doesIntegerFitInCurrentWord({boost::none, kMinDataBits, {0, 0, 0, 0}})) {
            // Form simple8b word if skip can not fit with last selector
            uint64_t simple8bWord = _encodeLargestPossibleWord(_lastValidExtensionType);
            writeFn(simple8bWord);
            _lastValidExtensionType = kBaseSelector;
        }

        if (_pendingValues.empty() && isLastValueSkip && tryRle) {
            // It is possible to start rle
            _rleCount = 1;
            _lastValueInPrevWord = boost::none;
            return;
        }
    }
    // Push true into skip and the dummy value, 0, into currNum. We use the dummy value, 0 because
    // it takes 1 bit and it will not affect our global curr bit length calculations.
    _pendingValues.push_back({boost::none, {0, 0, 0, 0}, {0, 0, 0, 0}});
}

template <typename T, class Allocator>
template <class F>
void Simple8bBuilder<T, Allocator>::_handleRleTermination(F&& writeFn) {
    if (_rleCount == 0)
        return;

    // Try to create a RLE Simple8b word.
    _appendRleEncoding(writeFn);
    // Add any values that could not be encoded in RLE.
    while (_rleCount > 0) {
        if (!_lastValueInPrevWord.has_value()) {
            _appendSkip(false /* tryRle */, writeFn);
        } else {
            _appendValue(_lastValueInPrevWord.value(), false, writeFn);
        }
        --_rleCount;
    }

    // Reset last for RLE and which selectors are possible to use for next word
    _lastValueInPrevWord = 0;
    isSelectorPossible.fill(true);
}

template <typename T, class Allocator>
template <class F>
void Simple8bBuilder<T, Allocator>::_appendRleEncoding(F&& writeFn) {
    using namespace simple8b_internal;

    // This encodes a value using rle. The selector is set as 15 and the count is added in the next
    // 4 bits. The value is the previous value stored by simple8b or 0 if no previous value was
    // stored.
    auto createRleEncoding = [&writeFn](uint8_t count) {
        uint64_t rleEncoding = kRleSelector;
        // We will store (count - 1) during encoding and execute (count + 1) during decoding.
        rleEncoding |= (count - 1) << kSelectorBits;
        writeFn(rleEncoding);
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

template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::_rlePossible() const {
    return _pendingValues.empty() || _rleCount != 0;
}


template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::_doesIntegerFitInCurrentWord(const PendingValue& value) {
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

template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::_doesIntegerFitInCurrentWordWithGivenSelectorType(
    const PendingValue& value, uint8_t extensionType) {
    using namespace simple8b_internal;

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

template <typename T, class Allocator>
int64_t Simple8bBuilder<T, Allocator>::_encodeLargestPossibleWord(uint8_t extensionType) {
    using namespace simple8b_internal;

    // Since this is always called right after _doesIntegerFitInCurrentWord fails for the first
    // time, we know all values in _pendingValues fits in the slots for the selector that can store
    // this many values. Find the smallest selector that doesn't leave any unused slots.
    uint8_t selector = getSelectorIndex(_pendingValues.size(), extensionType);
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

    // While we erase from the front of a vector the number of remaining elements is normally small.
    // Vector is a less complex data structure than a deque and normally has better performance when
    // the number of elements in it is low.
    _pendingValues.erase(_pendingValues.begin(), _pendingValues.begin() + integersCoded);
    _currMaxBitLen = kMinDataBits;
    for (const auto& val : _pendingValues) {
        _updateSimple8bCurrentState(val);
    }
    // Reset which selectors are possible to use for next word
    isSelectorPossible.fill(true);
    return encodedWord;
}

template <typename T, class Allocator>
template <typename Func>
uint64_t Simple8bBuilder<T, Allocator>::_encode(Func func,
                                                uint8_t selectorIdx,
                                                uint8_t extensionType) {
    using namespace simple8b_internal;

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

template <typename T, class Allocator>
void Simple8bBuilder<T, Allocator>::_updateSimple8bCurrentState(const PendingValue& val) {
    using namespace simple8b_internal;

    for (uint8_t i = 0; i < kNumOfSelectorTypes; ++i) {
        _currMaxBitLen[i] = std::max(_currMaxBitLen[i], val.bitCount[i]);
    }
}

template <typename T, class Allocator>
typename Simple8bBuilder<T, Allocator>::PendingIterator Simple8bBuilder<T, Allocator>::begin()
    const {
    return {_pendingValues.begin(), _pendingValues.begin(), _lastValueInPrevWord, _rleCount};
}

template <typename T, class Allocator>
typename Simple8bBuilder<T, Allocator>::PendingIterator Simple8bBuilder<T, Allocator>::end() const {
    return {_pendingValues.begin(), _pendingValues.end(), _lastValueInPrevWord, 0};
}

template <typename T, class Allocator>
std::reverse_iterator<typename Simple8bBuilder<T, Allocator>::PendingIterator>
Simple8bBuilder<T, Allocator>::rbegin() const {
    return std::reverse_iterator<typename Simple8bBuilder<T, Allocator>::PendingIterator>(end());
}

template <typename T, class Allocator>
std::reverse_iterator<typename Simple8bBuilder<T, Allocator>::PendingIterator>
Simple8bBuilder<T, Allocator>::rend() const {
    return std::reverse_iterator<typename Simple8bBuilder<T, Allocator>::PendingIterator>(begin());
}

template <typename T, class Allocator>
bool Simple8bBuilder<T, Allocator>::isInternalStateIdentical(
    const Simple8bBuilder<T, Allocator>& other) const {
    // Verifies the pending values
    if (!std::equal(begin(), end(), other.begin(), other.end())) {
        return false;
    }
    if (_rleCount != other._rleCount) {
        return false;
    }
    if (_lastValueInPrevWord != other._lastValueInPrevWord) {
        return false;
    }
    if (_currMaxBitLen != other._currMaxBitLen) {
        return false;
    }
    if (_currTrailingZerosCount != other._currTrailingZerosCount) {
        return false;
    }
    if (_lastValidExtensionType != other._lastValidExtensionType) {
        return false;
    }
    if (isSelectorPossible != other.isSelectorPossible) {
        return false;
    }
    return true;
}

}  // namespace mongo
