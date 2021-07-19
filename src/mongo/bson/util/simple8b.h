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

namespace mongo {

/**
 * Simple8b compresses a series of integers into chains of 64 bit Simple8b word.
 */
class Simple8b {
public:
    // Number of different type of selectors and their extensions available
    static constexpr uint8_t kNumOfSelectorTypes = 4;

    struct Value {
        uint32_t index;
        uint64_t val;
    };
    /**
     * Retrieves all integers in the order it was appended.
     */
    std::vector<Value> getAllInts();

    /**
     * Checks if we can append val to an existing RLE and handles the ending of a RLE.
     * The default RLE value at the beginning is 0.
     * Otherwise, Appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     */
    bool append(uint64_t val);

    /**
     * Appends an empty bucket to handle missing values. This works by incrementing an underlying
     * simple8b index by one and encoding a "missing value" in the simple8b block as all 1s.
     */
    void skip();

    /**
     * Stores all values for RLE or in _pendingValues into _buffered even if the Simple8b word will
     * not be opimtal and use a larger selector than necessary because we don't have enough integers
     * to use one wiht more slots.
     */
    void flush();

    /**
     * Returns the underlying binary encoding in _buffered.
     */
    char* data();

    /**
     * Returns the number of bytes in the binary buffer returned by the function, data().
     */
    size_t len();

    /**
     * This stores a value that has yet to be added to the buffer. It also stores the number of bits
     * required to store the value for each selector extension type. Furthermore, it stores the
     * number of trailing zeros that would be stored if this value was stored according to the
     * respective selector type. The arrays are indexed using the same selector indexes as defined
     * in the cpp file.
     */
    struct PendingValue {
        PendingValue(uint64_t val,
                     std::array<uint8_t, kNumOfSelectorTypes> bitCount,
                     std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount,
                     bool skip);
        uint64_t val;
        std::array<uint8_t, kNumOfSelectorTypes> bitCount = {0, 0, 0, 0};
        // This is not the total number of trailing zeros, but the trailing zeros that will be
        // stored given the selector chosen.
        std::array<uint8_t, kNumOfSelectorTypes> trailingZerosCount = {0, 0, 0, 0};
        bool skip;
    };

private:
    /**
     * Appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     */
    bool _appendValue(uint64_t value, bool tryRle);

    /**
     * Appends a skip to _pendingValues and forms a new Simple8b word if there i
  s no space.
     */
    void _appendSkip();

    /**
     * When an RLE ends because of inconsecutive values, check if there are enou
     gh
     * consecutive values for a RLE value and/or any values to be appended to _p
     endingValues.
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
     * Decodes a simple8b word into a vector of integers and their indices. It appends directly
     * into the passed in vector and the index values starts from the passed in index variable.
     * When the selector is invalid, nothing will be appended.
     */
    void _decode(const uint64_t simple8bWord,
                 uint32_t* index,
                 std::vector<Value>* decodedValues) const;

    /*
     * Parses a simple8b word with the selector compression method passed. This is a helper method
     * to _decode above which determines the selector and then passes the correpsonding functor to
     * this function for actual decoding of the simple8b word.
     */
    template <typename Func>
    void _parseWord(Func func,
                    uint8_t selector,
                    uint8_t extensionType,
                    const uint64_t simple8bWord,
                    uint32_t* index,
                    std::vector<Simple8b::Value>* decodedValues) const;

    /*
     * Decodes a RLE simple8b word. A helper method to _decode function.
     */
    void _rleDecode(const uint64_t simple8bWord,
                    uint32_t* index,
                    std::vector<Simple8b::Value>* decodedValues) const;

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
    PendingValue _lastValueInPrevWord = {0, {0, 0, 0, 0}, {0, 0, 0, 0}, false};

    // This buffer holds the simple8b compressed words
    BufBuilder _buffer;

    // These variables hold the max amount of bits for each value in _pendingValues. They are
    // updated whenever values are added or removed from _pendingValues to always reflect the max
    // value in the deque.
    std::array<uint8_t, kNumOfSelectorTypes> _currMaxBitLen = {0, 0, 0, 0};
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
};

}  // namespace mongo
