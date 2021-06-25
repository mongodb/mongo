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
     * Appends a value to the Simple8b chain of words.
     * Return true if successfully appended and false otherwise.
     */
    bool append(uint64_t val);

    /**
     * Appends an empty bucket to handle missing values. This works by incrementing an underlying
     * simple8b index by one and encoding a "missing value" in the simple8b block as all 1s.
     */
    void skip();

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
    void _decode(uint64_t simple8bWord, uint32_t* index, std::vector<Value>* decodedValues) const;

    /**
     * Takes a vector of integers to be compressed into a 64 bit word.
     * The values will be stored from right to left in little endian order.
     * If there are wasted bits, they will be placed at the very left.
     * For now, we will assume that all ints in the vector are greater or equal to zero.
     * We will also assume that the selector and all values will fit into the 64 bit word.
     * Returns the encoded Simple8b word if the inputs are valid and errCode otherwise.
     */
    uint64_t _encode(uint8_t selector, uint8_t endIdx);

    struct PendingValue {
        bool skip;
        uint64_t val;
    };

    BufBuilder _buffer;
    uint8_t _currMaxBitLen = 0;
    std::deque<PendingValue> _pendingValues;
};

}  // namespace mongo
