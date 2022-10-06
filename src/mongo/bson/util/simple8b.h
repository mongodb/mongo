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

#include <array>
#include <deque>
#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/platform/int128.h"

namespace mongo {

/**
 * Callback type to implement writing of 64 bit Simple8b words.
 */
using Simple8bWriteFn = std::function<void(uint64_t)>;

/**
 * Simple8bBuilder compresses a series of integers into chains of 64 bit Simple8b blocks.
 *
 * T may be uint64_t and uint128_t only.
 */
template <typename T>
class Simple8bBuilder {
private:
    struct PendingValue;

public:
    // Callback to handle writing of finalized Simple-8b blocks. Machine Endian byte order, the
    // value need to be converted to Little Endian before persisting.
    Simple8bBuilder(Simple8bWriteFn writeFunc = nullptr);
    ~Simple8bBuilder();

    /**
     * Appends val to Simple8b. Returns true if the append was successful and false if the value was
     * outside the range of possible values we can store in Simple8b.
     *
     * A call to append may result in multiple Simple8b blocks being finalized.
     */
    bool append(T val);

    /**
     * Appends a missing value to Simple8b.
     *
     * May result in a single Simple8b being finalized.
     */
    void skip();

    /**
     * Flushes all buffered values into finalized Simple8b blocks.
     *
     * It is allowed to continue to append values after this call.
     */
    void flush();

    /**
     * Iterator for reading pending values in Simple8bBuilder that has not yet been written to
     * Simple-8b blocks.
     *
     * Provides bidirectional iteration
     */
    class PendingIterator {
    public:
        friend class Simple8bBuilder;
        // typedefs expected in iterators
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = boost::optional<T>;
        using pointer = const boost::optional<T>*;
        using reference = const boost::optional<T>&;

        pointer operator->() const;
        reference operator*() const;

        PendingIterator& operator++();
        PendingIterator operator++(int);

        PendingIterator& operator--();
        PendingIterator operator--(int);

        bool operator==(const PendingIterator& rhs) const;
        bool operator!=(const PendingIterator& rhs) const;

    private:
        PendingIterator(typename std::deque<PendingValue>::const_iterator beginning,
                        typename std::deque<PendingValue>::const_iterator it,
                        reference rleValue,
                        uint32_t rleCount);

        typename std::deque<PendingValue>::const_iterator _begin;
        typename std::deque<PendingValue>::const_iterator _it;

        const boost::optional<T>& _rleValue;
        uint32_t _rleCount;
    };

    /**
     * Forward iterators to read pending values
     */
    PendingIterator begin() const;
    PendingIterator end() const;

    /**
     * Reverse iterators to read pending values
     */
    std::reverse_iterator<PendingIterator> rbegin() const;
    std::reverse_iterator<PendingIterator> rend() const;

    /**
     * Set write callback
     */
    void setWriteCallback(Simple8bWriteFn writer);

private:
    // Number of different type of selectors and their extensions available
    static constexpr uint8_t kNumOfSelectorTypes = 4;

    /**
     * This stores a value that has yet to be added to the buffer. It also stores the number of bits
     * required to store the value for each selector extension type. Furthermore, it stores the
     * number of trailing zeros that would be stored if this value was stored according to the
     * respective selector type. The arrays are indexed using the same selector indexes as defined
     * in the cpp file.
     */
    struct PendingValue {
        PendingValue() = default;
        PendingValue(boost::optional<T> val,
                     std::array<uint8_t, kNumOfSelectorTypes> bitCount,
                     std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount);

        bool isSkip() const {
            return !val.has_value();
        }

        T value() const {
            return val.value();
        }

        boost::optional<T> val = T{0};
        std::array<uint8_t, kNumOfSelectorTypes> bitCount = {0, 0, 0, 0};
        // This is not the total number of trailing zeros, but the trailing zeros that will be
        // stored given the selector chosen.
        std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount = {0, 0, 0, 0};
    };

    // The min number of meaningful bits each selector can store
    static constexpr std::array<uint8_t, 4> kMinDataBits = {1, 2, 4, 4};
    /**
     * Function objects to encode Simple8b blocks for the different extension types.
     *
     * See .cpp file for more information.
     */
    struct BaseSelectorEncodeFunctor;
    struct SevenSelectorEncodeFunctor;

    template <uint8_t ExtensionType>
    struct EightSelectorEncodeFunctor;

    struct EightSelectorSmallEncodeFunctor;
    struct EightSelectorLargeEncodeFunctor;

    /**
     * Appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     *
     * 'tryRle' indicates if we are allowed to put this skip in RLE count or not. Should only be set
     * to true when terminating RLE and we are flushing excess values.
     */
    bool _appendValue(T value, bool tryRle);

    /**
     * Appends a skip to _pendingValues and forms a new Simple8b word if there is no space.
     *
     * 'tryRle' indicates if we are allowed to put this value in RLE count or not. Should only be
     * set to true when terminating RLE and we are flushing excess values.
     */
    void _appendSkip(bool tryRle);

    /**
     * When an RLE ends because of inconsecutive values, check if there are enough
     * consecutive values for a RLE value and/or any values to be appended to _pendingValues.
     */
    void _handleRleTermination();

    /**
     * Based on _rleCount, create a RLE Simple8b word if possible.
     * If _rleCount is not large enough, do nothing.
     */
    void _appendRleEncoding();

    /*
     * Checks to see if RLE is possible and/or ongoing
     */
    bool _rlePossible() const;

    /**
     * Tests if a value would fit inside the current simple8b word using any of the selectors
     * selector. Returns true if adding the value fits in the current simple8b word and false
     * otherwise.
     */
    bool _doesIntegerFitInCurrentWord(const PendingValue& value);

    /*
     * This is a helper method for testing if a given selector will allow an integer to fit in a
     * simple8b word. Takes in a value to be stored and an extensionType representing the selector
     * compression method to check. Returns true if the word fits and updates the global
     * _lastValidExtensionType with the extensionType passed. If false, updates
     * isSelectorPossible[extensionType] to false so we do not need to recheck that extension if we
     * find a valid type and more values are added into the current word.
     */
    bool _doesIntegerFitInCurrentWordWithGivenSelectorType(const PendingValue& value,
                                                           uint8_t extensionType);

    /**
     * Encodes the largest possible simple8b word from _pendingValues without unused buckets using
     * the selector compression method passed in extensionType. Assumes is always called right after
     * _doesIntegerFitInCurrentWord fails for the first time. It removes the integers used to form
     * the simple8b word from _pendingValues permanently and updates our global state with any
     * remaining integers in _pendingValues.
     */
    int64_t _encodeLargestPossibleWord(uint8_t extensionType);

    /**
     * Takes a vector of integers to be compressed into a 64 bit word via the selector type given.
     * The values will be stored from right to left in little endian order.
     * For now, we will assume that all ints in the vector are greater or equal to zero.
     * We will also assume that the selector and all values will fit into the 64 bit word.
     * Returns the encoded Simple8b word if the inputs are valid and errCode otherwise.
     */
    template <typename Func>
    uint64_t _encode(Func func, uint8_t selectorIdx, uint8_t extensionType);

    /**
     * Updates the simple8b current state with the passed parameters. The maximum is always taken
     * between the current state and the new value passed. This is used to keep track of the size of
     * the simple8b word that we will need to encode.
     */
    void _updateSimple8bCurrentState(const PendingValue& val);

    // If RLE is ongoing, the number of consecutive repeats fo lastValueInPrevWord.
    uint32_t _rleCount = 0;
    // If RLE is ongoing, the last value in the previous Simple8b word.
    PendingValue _lastValueInPrevWord;

    // These variables hold the max amount of bits for each value in _pendingValues. They are
    // updated whenever values are added or removed from _pendingValues to always reflect the max
    // value in the deque.
    std::array<uint8_t, kNumOfSelectorTypes> _currMaxBitLen = kMinDataBits;
    std::array<uint8_t, kNumOfSelectorTypes> _currTrailingZerosCount = {0, 0, 0, 0};

    // This holds the last valid selector compression method that succeded for
    // doesIntegerFitInCurrentWord and is used to designate the compression type when we need to
    // write a simple8b word to buffer.
    uint8_t _lastValidExtensionType = 0;

    // Holds whether the selector compression method is possible. This is updated in
    // doesIntegerFitInCurrentWordWithSelector to avoid unnecessary calls when a selector is already
    // invalid for the current set of words in _pendingValues.
    std::array<bool, kNumOfSelectorTypes> isSelectorPossible = {true, true, true, true};

    // This holds values that have not be encoded to the simple8b buffer, but are waiting for a full
    // simple8b word to be filled before writing to buffer.
    std::deque<PendingValue> _pendingValues;

    // User-defined callback to handle writing of finalized Simple-8b blocks
    Simple8bWriteFn _writeFn;
};

/**
 * Simple8b provides an interface to read Simple8b encoded data built by Simple8bBuilder above
 */
template <typename T>
class Simple8b {
public:
    class Iterator {
    public:
        friend class Simple8b;

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

    private:
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

        const char* _pos;
        const char* _end;

        // Current Simple8b block in native endian
        uint64_t _current;

        boost::optional<T> _value;

        // Mask for getting a single Simple-8b slot
        uint64_t _mask;

        // Remaining RLE count for repeating previous value
        uint16_t _rleRemaining;

        // Number of positions to shift the mask to get slot for current iterator position
        uint8_t _shift;

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
    Simple8b(const char* buffer, int size, boost::optional<T> previous = T{});

    /**
     * Forward iterators to read decompressed values
     */
    Iterator begin() const;
    Iterator end() const;

private:
    const char* _buffer;
    int _size;
    // Previous value to be used in case the first block in the buffer is RLE.
    boost::optional<T> _previous;
};

}  // namespace mongo
