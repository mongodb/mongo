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

#include <deque>
#include <vector>

#include "mongo/bson/util/builder.h"

namespace mongo {

/**
 * Simple8b compresses a series of integers into chains of 64 bit Simple8b word.
 */
class Simple8b {
public:
    struct Value {
        uint32_t index;
        uint64_t val;
    };

    // TODO (SERVER-57808): Remove temporary error code.
    static constexpr uint64_t errCode = 0x0000000000000000;

    /**
     * Retrieves all integers in the order it was appended.
     */
    std::vector<Value> getAllInts();

    /**
     * Checks if we can append val to an existing RLE and handles the ending of a RLE.
     * The default RLE value at the beginning is 0.
     * Otherwise, appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     */
    bool append(uint64_t val);

    /**
     * Appends an empty bucket to handle missing values. This works by incrementing an underlying
     * simple8b index by one and encoding a "missing value" in the simple8b block as all 1s.
     */
    void skip();

    /**
     * Stores all values for RLE or in _pendingValues into _buffered even if the Simple8b word
     * will not be optimal and use a larger selector than necessary because we don't have
     * enough integers to use one with more slots.
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

private:
    /**
     * Tests if a value with numBits would fit inside the current simple8b word.
     * Returns true if adding the value fits in the current simple8b word and false otherwise.
     */
    bool _doesIntegerFitInCurrentWord(uint8_t numBits) const;

    /**
     * Encodes the largest possible simple8b word from _pendingValues without unused buckets.
     * Assumes is always called right after _doesIntegerFitInCurrentWord fails for the first time.
     * It removes the integers used to form the simple8b word from _pendingValues permanently
     * and updates _currMaxBitLen.
     */
    int64_t _encodeLargestPossibleWord();

    /**
     * Decodes a simple8b word into a vector of integers and their indices. It appends directly
     * into the passed in vector and the index values starts from the passed in index variable.
     * When the selector is invalid, nothing will be appended.
     */
    void _decode(uint64_t simple8bWord, uint32_t* index, std::vector<Value>* decodedValues);

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

    /**
     * Appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     */
    bool _appendValue(uint64_t value, bool tryRle);

    /**
     * Appends a skip to _pendingValues and forms a new Simple8b word if there is no space.
     */
    void _appendSkip();

    /**
     * Takes a vector of integers to be compressed into a 64 bit word.
     * The values will be stored from right to left in little endian order.
     * If there are wasted bits, they will be placed at the very left.
     * For now, we will assume that all ints in the vector are greater or equal to zero.
     * We will also assume that the selector and all values will fit into the 64 bit word.
     * Returns the encoded Simple8b word if the inputs are valid and errCode otherwise.
     */
    uint64_t _encode(uint8_t selector, uint8_t endIdx);

    /*
     * Checks to see if RLE is possible and/or ongoing.
     */
    bool _rlePossible();

    struct PendingValue {
        bool skip;
        uint64_t val;
    };

    // The number of bits used by the largest integer in _pendingValues.
    uint8_t _currMaxBitLen = 0;

    // If RLE is ongoing, the number of consecutive repeats of lastValueInPrevWord.
    uint32_t _rleCount = 0;
    // If RLE is ongoing, the last value in the previous Simple8b word.
    PendingValue _lastValueInPrevWord = {false, 0};

    // The binary buffer storing all completed Simple8b words.
    BufBuilder _buffer;
    // A buffer of values that have not been added to _buffer yet.
    std::deque<PendingValue> _pendingValues;
};

}  // namespace mongo
