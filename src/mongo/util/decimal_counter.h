/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdint>
#include <limits>

#include "mongo/base/string_data.h"

namespace mongo {
/**
 * Stores unsigned counter as well as its decimal ASCII representation, avoiding the need for
 * separate binary to decimal conversions. This speeds up code that needs string representations
 * for all counter values.
 */
template <typename T>
class DecimalCounter {
public:
    static_assert(std::is_unsigned<T>::value, "DecimalCounter requires an unsigned type");
    constexpr operator StringData() const {
        return {_digits, static_cast<size_t>(_lastDigitIndex + 1)};
    }
    constexpr operator uint32_t() const {
        return _counter;
    }

    /**
     *  Increments the counter and its decimal representation. The decimal representation wraps
     *  independently of the binary counter.
     */
    DecimalCounter<T>& operator++() {
        // Common case: just increment the last digit and we're done with the string part.
        char* lastPtr = _digits + _lastDigitIndex;
        if (MONGO_unlikely((*lastPtr)++ == '9')) {
            // Let zeroPtr point at the first char in the string, such that it and all digits
            // after need to change to zeroes.
            char* zeroPtr = lastPtr;
            while (zeroPtr > _digits && zeroPtr[-1] == '9')
                --zeroPtr;

            // If digits wasn't all nines, increment the first non-nine.
            if (zeroPtr > _digits) {
                zeroPtr[-1]++;
            } else if (lastPtr < _digits + sizeof(_digits) - 2) {
                // Rare case: new power of 10 increases string length, so start with a one.
                *zeroPtr++ = '1';
                _lastDigitIndex++;
                lastPtr++;
            }
            // Zero out the rest.
            do {
                *zeroPtr++ = '0';
            } while (zeroPtr <= lastPtr);
        }
        if (MONGO_unlikely(++_counter == 0))
            *this = {};
        return *this;
    }

    /**
     *  Post-inrement version of operator++. Typically slower than pre-increment due to the need
     *  to return the pre-image by value.
     */
    DecimalCounter<T> operator++(int) {
        auto pre = *this;
        operator++();
        return pre;
    }

private:
    // Add 1, because digit10 is 1 less than the maximum number of digits, and 1 for the final '\0'.
    static constexpr size_t kBufSize = std::numeric_limits<T>::digits10 + 2;
    char _digits[kBufSize] = {'0'};  // Remainder is zero-initialized.
    uint8_t _lastDigitIndex = 0;     // Indicates the last digit in _digits.
    T _counter = 0;
};
}  // namespace mongo
