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

namespace mongo {

namespace {
static constexpr uint8_t kRleSelector = 15;
static constexpr uint8_t kMaxSelector = 14;
static constexpr uint8_t kMaxRleCout = 16;
static constexpr uint8_t kMinSelector = 1;
static constexpr uint64_t kSelectorMask = 0x000000000000000F;
static constexpr uint8_t kSelectorBits = 4;
static constexpr uint8_t kDataBits = 60;
static constexpr uint8_t kRleMinSize = 120;

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

void _createRleEncoding(uint8_t count, BufBuilder& buffer) {
    uint64_t rleEncoding = kRleSelector;
    // We will store (count - 1) during encoding and execute (count + 1) during decoding.
    rleEncoding |= (count - 1) << kSelectorBits;
    buffer.appendNum(tagLittleEndian(rleEncoding));
}

}  // namespace

std::vector<Simple8b::Value> Simple8b::getAllInts() {
    std::vector<Simple8b::Value> values;

    // Add integers in the BufBuilder first.
    uint64_t* buf = reinterpret_cast<uint64_t*>(_buffer.buf());

    size_t numBufWords = _buffer.len() / 8;  // 8 chars in a Simple8b word.
    uint32_t index = 0;
    for (size_t i = 0; i < numBufWords; i++) {
        uint64_t simple8bWord = LittleEndian<uint64_t>::load(*buf);
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

        values.push_back({index, _pendingValues[pendingValuesIdx].val});
        ++index;
    }

    if (_rleCount > 0) {
        if ((values.empty() && index != 0) ||
            (!values.empty() && values.back().index + 1 < index)) {
            // The last value in the previous word is a skip. Do not add anything.
            return values;
        }

        // If RLE started on the first Simple8b word, use default value 0.
        uint64_t value = 0;
        if (!values.empty()) {
            // There was a previous word, use the last value in that word (that is not a skip).
            value = values.back().val;
        }

        for (uint32_t i = 0; i < _rleCount; ++i) {
            // Then add all repeating integers that have been kept for RLE.
            values.push_back({index, value});
            ++index;
        }
    }

    return values;
}

bool Simple8b::append(uint64_t value) {
    // TODO (SERVER-58520): Remove invariants before release.
    // There should be no values in _pendingValues if RLE is happening and vice versa.
    invariant(_rleCount == 0 || _pendingValues.empty());

    if (_rlePossible()) {
        if (_lastValueInPrevWord.val == value) {
            // We can continue to build the RLE word.
            ++_rleCount;
            return true;
        }
        _handleRleTermination();
    }

    // RLE is not possible in the current word. Add current word normally.
    return _appendValue(value, true);
}

void Simple8b::skip() {
    // TODO (SERVER-58520): Remove invariants before release.
    // There should be no values in _pendingValues if RLE is happening and vice versa.
    invariant(_rleCount == 0 || _pendingValues.empty());

    if (_rlePossible() && _lastValueInPrevWord.skip) {
        ++_rleCount;
        return;
    }

    _handleRleTermination();
    _appendSkip();
}

void Simple8b::flush() {
    // TODO (SERVER-58520): Remove invariants before release.
    // There should be no values in _pendingValues if RLE is happening and vice versa.
    invariant(_rleCount == 0 || _pendingValues.empty());

    // Flush repeating integers that have been kept for RLE.
    _handleRleTermination();

    // Flush buffered values in _pendingValues.
    if (!_pendingValues.empty()) {
        PendingValue lastPendingValue = _pendingValues.back();
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord();
            _buffer.appendNum(tagLittleEndian(simple8bWord));
        } while (!_pendingValues.empty());

        // There are no more words in _pendingValues and RLE is possible.
        // However, the _rleCount is 0 because we have not read any of the values in the next word.
        _rleCount = 0;
        _lastValueInPrevWord = lastPendingValue;
    }
}

char* Simple8b::data() {
    return _buffer.buf();
}

size_t Simple8b::len() {
    return _buffer.len();  // Number of hex characters in a char.
}

bool Simple8b::_doesIntegerFitInCurrentWord(uint8_t numBits) const {
    uint64_t numBitsWithValue = std::max(_currMaxBitLen, numBits) * (_pendingValues.size() + 1);

    // We can compare with kDataBits in all cases even when we have fewer data bits in the case
    // of selector 7 and 8. Because their slot size is not divisible with 60 and uses the closest
    // smaller integer. One more would be strictly more than 60 making this comparison safe.
    return kDataBits >= numBitsWithValue;
}

int64_t Simple8b::_encodeLargestPossibleWord() {
    // TODO (SERVER-58520): Remove invariants before release.
    // Check that all values in _pendingValues can fit in a single Simple8b word.
    if (_pendingValues.size() == 7 || _pendingValues.size() == 8) {
        invariant(kDataBits - 4 >= _pendingValues.size() * _currMaxBitLen);
    } else {
        invariant(kDataBits >= _pendingValues.size() * _currMaxBitLen);
    }

    // Since this is always called right after _doesIntegerFitInCurrentWord fails for the first
    // time, we know all values in _pendingValues fits in the slots for the selector that can store
    // this many values. Find the smallest selector that doesn't leave any unused slots.
    uint8_t selector = [&] {
        for (int s = kMinSelector; s <= kMaxSelector; ++s) {
            if (_pendingValues.size() >= _intsCodedForSelector[s])
                return s;
        }

        // TODO (SERVER-57808): The only edge case is if the value requires more than 60 bits.
        return 1;
    }();

    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t encodedWord = _encode(selector, integersCoded);

    _pendingValues.erase(_pendingValues.begin(), _pendingValues.begin() + integersCoded);
    _currMaxBitLen = _pendingValues.empty()
        ? 0
        : _countBits((*std::max_element(_pendingValues.begin(),
                                        _pendingValues.end(),
                                        [](const PendingValue& lhs, const PendingValue& rhs) {
                                            return _countBits(lhs.val) < _countBits(rhs.val);
                                        }))
                         .val);

    return encodedWord;
}

void Simple8b::_decode(const uint64_t simple8bWord,
                       uint32_t* index,
                       std::vector<Simple8b::Value>* decodedValues) {
    uint8_t selector = simple8bWord & kSelectorMask;
    if (selector < kMinSelector)
        return;

    if (selector == kRleSelector) {
        // RLE case.
        uint64_t mask = kSelectorMask << kSelectorBits;
        uint8_t count = (simple8bWord & mask) >> kSelectorBits;

        if (*index == 0) {
            // We stored (count - 1) during encoding, so we use (count + 1) during decoding.
            for (uint32_t i = 0; i < (uint32_t)(count + 1) * kRleMinSize; ++i) {
                // If there is a RLE as the first value, store 0's.
                decodedValues->push_back({*index, 0});
                ++(*index);
            }
        }

        if (*index > decodedValues->back().index + 1) {
            // The previous value was skip.
            (*index) += (count + 1) * kRleMinSize;
            return;
        }

        uint64_t previousValue = decodedValues->back().val;
        // We stored (count - 1) during encoding, so we use (count + 1) during decoding.
        for (uint32_t i = 0; i < (uint32_t)(count + 1) * kRleMinSize; ++i) {
            decodedValues->push_back({*index, previousValue});
            ++(*index);
        }

        return;
    }

    uint8_t bitsPerInteger = _bitsPerIntForSelector[selector];
    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t unshiftedMask = _maskForSelector[selector];

    for (uint8_t integerIdx = 0; integerIdx < integersCoded; ++integerIdx) {
        uint8_t startIdx = bitsPerInteger * integerIdx + kSelectorBits;

        uint64_t mask = unshiftedMask << startIdx;
        uint64_t value = (simple8bWord & mask) >> startIdx;

        if (unshiftedMask != value) {
            // value is not skip.
            decodedValues->push_back({*index, value});
        }

        ++(*index);
    }
}

void Simple8b::_handleRleTermination() {
    if (_rleCount == 0)
        return;

    // Try to create a RLE Simple8b word.
    _appendRleEncoding();

    // Add any values that could not be encoded in RLE.
    while (_rleCount > 0) {
        if (_lastValueInPrevWord.skip) {
            _appendSkip();
        } else {
            _appendValue(_lastValueInPrevWord.val, false);
        }
        --_rleCount;
    }
}


void Simple8b::_appendRleEncoding() {
    uint8_t count = _rleCount / kRleMinSize;

    if (count < 1) {
        // Insufficent count to create RLE encoding.
        return;
    }

    while (count > kMaxRleCout) {
        // If one RLE word is insufficient, use multiple RLE words.
        _createRleEncoding(kMaxRleCout, _buffer);
        count -= kMaxRleCout;
    }

    _createRleEncoding(count, _buffer);

    _rleCount %= kRleMinSize;
}


bool Simple8b::_appendValue(uint64_t value, bool tryRle) {
    uint8_t valueNumBits = _countBits(value);

    // Check if the amount of bits needed is more than the largest selector allows.
    if (countLeadingZeros64(value) < kSelectorBits)
        return false;

    if (_doesIntegerFitInCurrentWord(valueNumBits)) {
        // If the integer fits in the current word, add it and update global variables if necessary.
        _currMaxBitLen = std::max(_currMaxBitLen, valueNumBits);
        _pendingValues.push_back({false, value});
    } else {
        // If the integer does not fit in the current word, convert the integers into simple8b
        // word(s) with no unused buckets until the new value can be added to _pendingValues. Then
        // add the Simple8b word(s) to the buffer. Finally add the new integer and update any global
        // variables.
        PendingValue lastPendingValue = _pendingValues.back();
        do {
            uint64_t simple8bWord = _encodeLargestPossibleWord();
            _buffer.appendNum(tagLittleEndian(simple8bWord));
        } while (!_doesIntegerFitInCurrentWord(valueNumBits));

        if (tryRle && _pendingValues.empty() && lastPendingValue.val == value) {
            // There are no more words in _pendingValues and the last element of the last Simple8b
            // word is the same as the new value. Therefore, start RLE.
            _rleCount = 1;
            _lastValueInPrevWord = lastPendingValue;
        } else {
            _pendingValues.push_back({false, value});
            _currMaxBitLen = std::max(_currMaxBitLen, valueNumBits);
        }
    }

    return true;
}

void Simple8b::_appendSkip() {
    if (!_pendingValues.empty()) {
        bool isLastValueSkip = _pendingValues.back().skip;

        // There is never a case where we need to write more than one Simple8b word
        // because we only need 1 bit for skip.
        if (!_doesIntegerFitInCurrentWord(1)) {
            // Form Simple8b word if skip can not fit.
            uint64_t simple8bWord = _encodeLargestPossibleWord();
            _buffer.appendNum(tagLittleEndian(simple8bWord));
        }

        if (_pendingValues.empty() && isLastValueSkip) {
            // It is possible to start RLE.
            _rleCount = 1;
            _lastValueInPrevWord = {true, 0};
            return;
        }
    }

    // Push true into skip and the dummy value, 0, into currNum. We use the dummy value, 0,
    // because it takes 1 bit and it will not affect _currMaxBitLen calculations.
    _pendingValues.push_back({true, 0});
}

uint64_t Simple8b::_encode(uint8_t selector, uint8_t endIdx) {
    // TODO (SERVER-57808): create global error code.
    if (selector > kRleSelector || selector < kMinSelector)
        return errCode;

    uint8_t bitsPerInteger = _bitsPerIntForSelector[selector];
    uint8_t integersCoded = _intsCodedForSelector[selector];
    uint64_t unshiftedMask = _maskForSelector[selector];

    uint64_t encodedWord = selector;
    for (uint8_t i = 0; i < integersCoded; ++i) {
        uint8_t shiftSize = bitsPerInteger * i + kSelectorBits;
        uint64_t currEncodedWord = _pendingValues[i].skip ? unshiftedMask : _pendingValues[i].val;

        encodedWord |= currEncodedWord << shiftSize;
    }

    return encodedWord;
}

bool Simple8b::_rlePossible() {
    return _pendingValues.empty() || _rleCount != 0;
}

}  // namespace mongo
