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

#include "mongo/platform/bits.h"

namespace mongo {

namespace {
static constexpr uint8_t _maxSelector = 14;  // TODO (SERVER-57794): Change to 15.
static constexpr uint8_t _minSelector = 1;
static constexpr uint64_t _selectorMask = 0x000000000000000F;
static constexpr uint8_t _selectorBits = 4;
static constexpr uint8_t _dataBits = 60;

// Pass the selector as the index to get the corresponding mask.
// Get the maskSize by getting the number of bits for the selector. Then 2^maskSize - 1.
constexpr uint64_t _maskForSelector[16] = {0,
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
                                           1};


// Pass the selector value as the index to get the number of bits per integer in the Simple8b block.
constexpr uint8_t _bitsPerIntForSelector[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60, 0};

// Pass the selector value as the index to get the number of integers coded in the Simple8b block.
constexpr uint8_t _intsCodedForSelector[16] = {
    120, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1, 1};

uint8_t _countBits(uint64_t value) {
    // All 1s is reserved for skip encoding, so we add 1 to value to account for that case.
    return 64 - countLeadingZeros64(value + 1);
}

}  // namespace

std::vector<std::pair<uint32_t, uint64_t>> Simple8b::getAllInts() {
    std::vector<std::pair<uint32_t, uint64_t>> values;

    // Add integers in the BufBuilder first.
    SharedBuffer sb = _buffer.release();
    uint64_t* buf = reinterpret_cast<uint64_t*>(sb.get());

    size_t numBufWords = _buffer.len() / 8;  // 8 chars in a Simple8b word.
    uint32_t index = 0;
    for (size_t i = 0; i < numBufWords; i++) {
        uint64_t simple8bWord = *buf;
        _decode(simple8bWord, &index, &values);

        buf++;
    }

    // Then add buffered integers that has not yet been written to the buffer.
    for (size_t pendingValuesIdx = 0; pendingValuesIdx < _pendingValues.size();
         ++pendingValuesIdx) {
        if (_pendingValues[pendingValuesIdx].skip) {
            ++index;
            continue;
        }

        values.push_back(std::make_pair(index, _pendingValues[pendingValuesIdx].val));
        ++index;
    }

    return values;
}

bool Simple8b::append(uint64_t value) {
    uint8_t valueNumBits = _countBits(value);

    // Check if the amount of bits needed is more than the largest selector allows.
    if (countLeadingZeros64(value) < _selectorBits)
        return false;

    if (_doesIntegerFitInCurrentWord(value)) {
        // If the integer fits in the current word, add it and update global variables if necessary.
        _currMaxBitLen = std::max(_currMaxBitLen, valueNumBits);
        _pendingValues.emplace_back(false, value);
    } else {
        // If the integer does not fit in the current word, convert the integers into simple8b
        // word(s) with no unused buckets until the new value can be added to _pendingValues. Then
        // add the Simple8b word(s) to the buffer. Finally add the new integer and update any global
        // variables.
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord();
            _buffer.appendNum(simple8bWord);
        } while (!_doesIntegerFitInCurrentWord(value));

        _pendingValues.emplace_back(false, value);
        _currMaxBitLen =
            _countBits((*std::max_element(_pendingValues.begin(),
                                          _pendingValues.end(),
                                          [](PendingValue& lhs, PendingValue& rhs) {
                                              return _countBits(lhs.val) < _countBits(rhs.val);
                                          }))
                           .val);
    }

    return true;
}

void Simple8b::skip() {
    // Push true into skip and the dummy value, 0, into currNum. We use the dummy value, 0,
    // because it takes 1 bit and it will not affect _currMaxBitLen calculations.
    _pendingValues.emplace_back(true, 0);
}

bool Simple8b::_doesIntegerFitInCurrentWord(uint64_t value) const {
    uint8_t valueNumBits = _countBits(value);
    uint8_t numBitsWithNewInt = _currMaxBitLen < valueNumBits ? valueNumBits : _currMaxBitLen;
    numBitsWithNewInt *= (_pendingValues.size() + 1);

    return _dataBits >= numBitsWithNewInt;
}

int64_t Simple8b::_encodeLargestPossibleWord() {
    std::vector<uint8_t> maxBitsSoFarVec;

    // Store the max number of bits up to each point in the _pendingValues.
    // Use dynamic programming to make determining selector faster.
    maxBitsSoFarVec.push_back(_countBits(_pendingValues[0].val));
    for (size_t i = 1; i < _pendingValues.size(); ++i) {
        maxBitsSoFarVec.push_back(
            std::max(_countBits(_pendingValues[i].val), maxBitsSoFarVec[i - 1]));
    }

    // Determine best selector value.
    uint8_t selector = [&] {
        for (int i = 0; i <= _maxSelector; ++i) {
            if (_isSelectorValid(i, maxBitsSoFarVec[i]))
                return i;
        }

        // TODO (SERVER-57808): The only edge case is if the value requires more than 60 bits.
        return 1;
    }();

    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t encodedWord = _encode(selector, integersCoded);
    _pendingValues.erase(_pendingValues.begin(), _pendingValues.begin() + integersCoded);

    return encodedWord;
}

bool Simple8b::_isSelectorValid(uint8_t selector, uint8_t maxBitsSoFar) const {
    uint8_t numInts = _intsCodedForSelector[selector];
    bool areNoTrailingUnusedBuckets = _pendingValues.size() >= numInts;
    bool doesIntegersFitInBuckets = maxBitsSoFar <= _bitsPerIntForSelector[selector];
    return areNoTrailingUnusedBuckets && doesIntegersFitInBuckets;
}

void Simple8b::_decode(const uint64_t simple8bWord,
                       uint32_t* index,
                       std::vector<std::pair<uint32_t, uint64_t>>* decodedValues) const {
    uint8_t selector = simple8bWord & _selectorMask;
    if (selector < _minSelector)
        return;

    uint8_t bitsPerInteger = _bitsPerIntForSelector[selector];
    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t unshiftedMask = _maskForSelector[selector];

    for (uint8_t integerIdx = 0; integerIdx < integersCoded; ++integerIdx) {
        uint8_t startIdx = bitsPerInteger * integerIdx + _selectorBits;

        uint64_t mask = unshiftedMask << startIdx;
        uint64_t value = (simple8bWord & mask) >> startIdx;

        // If it is not a skip encoding, add it to the decoded values.
        if (unshiftedMask != value)
            decodedValues->push_back(std::make_pair(*index, value));

        ++(*index);
    }
}

uint64_t Simple8b::_encode(uint8_t selector, uint8_t endIdx) {
    // TODO (SERVER-57808): create global error code.
    if (selector > _maxSelector || selector < _minSelector)
        return errCode;

    uint8_t bitsPerInteger = _bitsPerIntForSelector[selector];
    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t unshiftedMask = _maskForSelector[selector];

    uint64_t encodedWord = selector;
    for (uint8_t i = 0; i < integersCoded; ++i) {
        uint8_t shiftSize = bitsPerInteger * i + _selectorBits;
        uint64_t currEncodedWord = _pendingValues[i].skip ? unshiftedMask : _pendingValues[i].val;

        encodedWord |= currEncodedWord << shiftSize;
    }

    return encodedWord;
}

}  // namespace mongo
