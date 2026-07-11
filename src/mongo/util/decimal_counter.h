// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/itoa.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * Stores unsigned counter as well as its decimal ASCII representation, avoiding the need for
 * separate binary to decimal conversions. This speeds up code that needs string representations
 * for all counter values.
 */
template <typename T>
class DecimalCounter {
public:
    static_assert(std::is_unsigned<T>::value, "DecimalCounter requires an unsigned type");

    DecimalCounter(T start = 0) : _lastDigitIndex(_getLastDigitIndex(start)), _counter(start) {}

    constexpr operator std::string_view() const {
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

private:
    uint8_t _getLastDigitIndex(T start) {
        if (!start) {
            return 0;
        }
        ItoA startItoA(start);
        std::string_view startStr(startItoA);
        std::memcpy(_digits, startStr.data(), startStr.size());
        return startStr.size() - 1;
    }

    // Add 1, because digit10 is 1 less than the maximum number of digits, and 1 for the final '\0'.
    static constexpr size_t kBufSize = std::numeric_limits<T>::digits10 + 2;
    char _digits[kBufSize] = {'0'};  // Remainder is zero-initialized.
    uint8_t _lastDigitIndex;         // Indicates the last digit in _digits.
    T _counter;
};
}  // namespace mongo
