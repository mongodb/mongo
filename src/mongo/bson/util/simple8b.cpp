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

namespace mongo {

namespace {
static constexpr uint8_t _maxSelector = 14;  // TODO (SERVER-57794): Change to 15.
static constexpr uint8_t _minSelector = 1;
static constexpr uint64_t _selectorMask = 0x000000000000000F;
static constexpr uint8_t _selectorSize = 4;

// Pass the selector value as the index to get the number of bits per integer in the Simple8b block.
constexpr uint8_t _selectorForBitsPerInteger[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60, 0};

// Pass the selector value as the index to get the number of integers coded in the Simple8b block.
constexpr uint8_t _selectorForIntegersCoded[16] = {
    120, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1, 1};

// Pass the selector as the index to get the corresponding mask.
// Get the maskSize by getting the number of bits for the selector. Then 2^maskSize - 1.
constexpr uint64_t _selectorForMask[16] = {0,
                                           1,
                                           2,
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
                                           1};

}  // namespace

uint64_t Simple8b::encodeSimple8b(uint8_t selector, const std::vector<uint64_t>& values) {
    if (selector > _maxSelector || selector < _minSelector)
        return errCode;

    uint8_t bitsPerInteger = _selectorForBitsPerInteger[selector];
    uint8_t integersCoded = _selectorForIntegersCoded[selector];

    uint64_t encodedWord = selector;
    for (uint8_t i = 0; i < integersCoded; ++i) {
        uint8_t shiftSize = bitsPerInteger * i + _selectorSize;
        encodedWord += values[i] << shiftSize;
    }

    return encodedWord;
}


std::vector<uint64_t> Simple8b::decodeSimple8b(const uint64_t simple8bWord) {
    std::vector<uint64_t> values;

    uint8_t selector = simple8bWord & _selectorMask;

    if (selector < _minSelector)
        return values;

    uint8_t bitsPerInteger = _selectorForBitsPerInteger[selector];
    uint8_t integersCoded = _selectorForIntegersCoded[selector];

    for (int8_t i = 0; i < integersCoded; ++i) {
        uint8_t startIdx = bitsPerInteger * i + _selectorSize;

        uint64_t mask = _selectorForMask[selector] << startIdx;
        uint64_t value = (simple8bWord & mask) >> startIdx;

        values.push_back(value);
    }

    return values;
}

}  // namespace mongo
