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

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/column/simple8b_helpers.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Simple8b provides an interface to read Simple8b encoded data built by Simple8bBuilder
 */
template <typename T>
class Simple8b {
public:
    class Iterator {
    public:
        friend class Simple8b;

        Iterator() = default;

        // typedefs expected in iterators
        using iterator_category = std::input_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = boost::optional<T>;
        using pointer = const boost::optional<T>*;
        using reference = const boost::optional<T>&;

        /**
         * Returns the number of values in the current Simple8b block that the iterator is
         * positioned on.
         */
        size_t blockSize() const;

        /**
         * Returns the value in at the current iterator position.
         */
        pointer operator->() const {
            return &_value;
        }
        reference operator*() const {
            return _value;
        }

        /**
         * Advance the iterator one step.
         */
        Iterator& operator++();

        /**
         * Advance the iterator to the next Simple8b block.
         */
        Iterator& advanceBlock();

        bool operator==(const Iterator& rhs) const;
        bool operator!=(const Iterator& rhs) const;

        /**
         * Returns true if iterator can be incremented. Equivalent to comparing not equal with the
         * end iterator.
         */
        bool more() const;

        /**
         * Returns true if iterator was instantiated with a valid memory block.
         */
        bool valid() const;

    private:
        Iterator(const char* end);
        Iterator(const char* pos, const char* end, const boost::optional<T>& previous);

        /**
         * Loads the current Simple8b block into the iterator
         */
        void _loadBlock();
        void _loadValue();

        /**
         * RLE count, may only be called if iterator is positioned on an RLE block
         */
        uint16_t _rleCountInCurrent(uint8_t selectorExtension) const;

        const char* _pos = nullptr;
        const char* _end = nullptr;

        // Current Simple8b block in native endian
        uint64_t _current;

        boost::optional<T> _value;

        // Mask for getting a single Simple-8b slot
        uint64_t _mask;

        // Remaining RLE count for repeating previous value
        uint16_t _rleRemaining = 0;

        // Number of positions to shift the mask to get slot for current iterator position
        uint8_t _shift = 0;

        // Number of bits in single Simple-8b slot, used to increment _shift when updating iterator
        // position
        uint8_t _bitsPerValue;

        // Variables for the extended Selectors 7 and 8 with embedded count in Simple-8b slot
        // Mask to extract count
        uint8_t _countMask;

        // Number of bits for the count
        uint8_t _countBits;

        // Multiplier of the value in count to get number of zeros
        uint8_t _countMultiplier;

        // Holds the current simple8b block's selector
        uint8_t _selector;

        // Holds the current simple8b blocks's extension type
        uint8_t _extensionType;
    };

    /**
     * Does not take ownership of buffer, must remain valid during the lifetime of this class.
     */
    Simple8b() = default;
    Simple8b(const char* buffer, int size, boost::optional<T> previous = T{});

    /**
     * Forward iterators to read decompressed values
     */
    Iterator begin() const;
    Iterator end() const;

private:
    const char* _buffer = nullptr;
    int _size = 0;
    // Previous value to be used in case the first block in the buffer is RLE.
    boost::optional<T> _previous = boost::none;
};

template <typename T>
Simple8b<T>::Iterator::Iterator(const char* end)
    : _pos(end), _end(end), _rleRemaining(0), _shift(0) {}

template <typename T>
Simple8b<T>::Iterator::Iterator(const char* pos,
                                const char* end,
                                const boost::optional<T>& previous)
    : _pos(pos), _end(end), _value(previous), _rleRemaining(0), _shift(0) {
    _loadBlock();
}

template <typename T>
void Simple8b<T>::Iterator::_loadBlock() {
    using namespace simple8b_internal;

    _current = ConstDataView(_pos).read<LittleEndian<uint64_t>>();

    _selector = _current & kBaseSelectorMask;
    uassert(8787300, "invalid selector 0", _selector);
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
        uassert(8787301, "invalid extended selector", _extensionType != kInvalidSelector);
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

    // It is undefined behavior to bit shift values by more than their bit length. While no sensible
    // encoders will produce these values, we must still define this behavior for UBSAN.
    auto numZeroes = trailingZeros * _countMultiplier;
    if constexpr (std::is_same<T, uint64_t>::value) {
        numZeroes %= 64;
    }

    _value = static_cast<T>((value >> _countBits)) << (numZeroes);
}

template <typename T>
size_t Simple8b<T>::Iterator::blockSize() const {
    using namespace simple8b_internal;

    if (_selector == kRleSelector) {
        uint8_t selectorExtension = (_current >> kSelectorBits) & kBaseSelectorMask;
        return _rleCountInCurrent(selectorExtension);
    }
    return kIntsStoreForSelector[_extensionType][_selector];
}

template <typename T>
uint16_t Simple8b<T>::Iterator::_rleCountInCurrent(uint8_t selectorExtension) const {
    using namespace simple8b_internal;
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
bool Simple8b<T>::Iterator::more() const {
    return _pos != _end;
}

template <typename T>
bool Simple8b<T>::Iterator::valid() const {
    return _pos != nullptr;
}

template <typename T>
Simple8b<T>::Simple8b(const char* buffer, int size, boost::optional<T> previous)
    : _buffer(buffer), _size(size), _previous(previous) {}

template <typename T>
typename Simple8b<T>::Iterator Simple8b<T>::begin() const {
    if (_size == 0) {
        return {_buffer};
    }
    return {_buffer, _buffer + _size, _previous};
}

template <typename T>
typename Simple8b<T>::Iterator Simple8b<T>::end() const {
    return {_buffer + _size};
}

namespace simple8b {
// Constant for a simple8b block containing a single 'missing' value.
static constexpr uint64_t kSingleSkip = 0xFFFFFFFFFFFFFFFE;

// Constant for a simple8b block containing a single zero value.
static constexpr uint64_t kSingleZero = 0xE;

/**
 * Visits all values in sequence with provided callbacks
 * visit - a callback for receiving all non-missing values (including 0)
 * visitMissing - a callback for receiving missing
 * visitZero - should have identical behavior to visit(0), may be used
 *             to provide a more efficient implementation of this case
 * Returns number of elements visited
 */
template <typename T, typename Visit, typename VisitZero, typename VisitMissing>
inline size_t visitAll(const char* buffer,
                       size_t size,
                       uint64_t& prevNonRLE,
                       const Visit& visit,
                       const VisitZero& visitZero,
                       const VisitMissing& visitMissing);

template <typename T, typename Visit, typename VisitMissing>
MONGO_COMPILER_ALWAYS_INLINE_GCC14 inline size_t visitAll(const char* buffer,
                                                          size_t size,
                                                          uint64_t& prevNonRLE,
                                                          const Visit& visit,
                                                          const VisitMissing& visitMissing) {
    return visitAll<T>(buffer, size, prevNonRLE, visit, [&visit]() { visit(0); }, visitMissing);
}

/**
 * Returns the total number of values over multiple Simple8b blocks including missing values.
 */
inline size_t count(const char* buffer, size_t size);

/**
 * Calculates the sum for multiple simple8b blocks in a buffer. 'prevNonRLE' should be initialized
 * to 'kSingleSkip' when calculating sum for the first buffer. If the caller needs sum from multiple
 * buffers the value should be passed along between the calls.
 */
template <typename T>
T sum(const char* buffer, size_t size, uint64_t& prevNonRLE);

/**
 * Calculates the prefix sum (or cumulative sum) for multiple simple8b blocks in a buffer. 'prefix'
 * should contain the initial prefix and will be updated to the new prefix after calculating prefix
 * sum for this block. 'prevNonRLE' should be initialized to 'kSingleSkip' when calculating prefix
 * sum for the first buffer. If the caller needs prefix sum from multiple buffers the values of
 * 'prefix' and 'prevNonRLE' should be passed along between the calls.
 */
template <typename T>
T prefixSum(const char* buffer, size_t size, T& prefix, uint64_t& prevNonRLE);

/**
 * Helper that performs signed addition as unsigned to get the defined wrap-around overflow behavior
 * rather than overflow, which is undefined behavior.
 */
static constexpr int64_t add(int64_t lhs, int64_t rhs) {
    return static_cast<int64_t>(static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs));
}
static constexpr int128_t add(int128_t lhs, int128_t rhs) {
    return static_cast<int128_t>(static_cast<uint128_t>(lhs) + static_cast<uint128_t>(rhs));
}

}  // namespace simple8b

}  // namespace mongo

#include "simple8b.inl"
