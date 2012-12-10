/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/base/parse_number.h"

#include <limits>

#include "mongo/platform/cstdint.h"

namespace mongo {

    /**
     * Returns the value of the digit "c", with the same conversion behavior as strtol.
     *
     * Assumes "c" is an ASCII character or UTF-8 octet.
     */
    static uint8_t _digitValue(char c) {
        if (c >= '0' && c <= '9')
            return uint8_t(c - '0');
        if (c >= 'a' && c <= 'z')
            return uint8_t(c - 'a' + 10);
        if (c >= 'A' && c <= 'Z')
            return uint8_t(c - 'A' + 10);
        return 36;  // Illegal digit value for all supported bases.
    }

    /**
     * Assuming "stringValue" represents a parseable number, extracts the sign and returns a
     * substring with any sign characters stripped away.  "*isNegative" is set to true if the
     * number is negative, and false otherwise.
     */
    static inline StringData _extractSign(const StringData& stringValue, bool* isNegative) {
        if (stringValue.empty()) {
            *isNegative = false;
            return stringValue;
        }

        bool foundSignMarker;
        switch (stringValue[0]) {
        case '-':
            foundSignMarker = true;
            *isNegative = true;
            break;
        case '+':
            foundSignMarker = true;
            *isNegative = false;
            break;
        default:
            foundSignMarker = false;
            *isNegative = false;
            break;
        }

        if (foundSignMarker)
            return stringValue.substr(1);
        return stringValue;
    }

    /**
     * Assuming "stringValue" represents a parseable number, determines what base to use given
     * "inputBase".  Stores the correct base into "*outputBase".  Follows strtol rules.  If
     * "inputBase" is not 0, *outputBase is set to "inputBase".  Otherwise, if "stringValue" starts
     * with "0x" or "0X", sets outputBase to 16, or if it starts with 0, sets outputBase to 8.
     *
     * Returns the substring of "stringValue" with the base-indicating prefix stripped off.
     */
    static inline StringData _extractBase(
            const StringData& stringValue, int inputBase, int* outputBase) {

        if (inputBase == 0) {
            if (stringValue.size() == 0) {
                *outputBase = inputBase;
                return stringValue;
            }
            if (stringValue[0] == '0') {
                if (stringValue.size() > 1 && (stringValue[1] == 'x' || stringValue[1] == 'X')) {
                    *outputBase = 16;
                    return stringValue.substr(2);
                }
                *outputBase = 8;
                return stringValue.substr(1);
            }
            *outputBase = 10;
            return stringValue;
        }
        else {
            *outputBase = inputBase;
            if (inputBase == 16) {
                StringData prefix = stringValue.substr(0, 2);
                if (prefix == "0x" || prefix == "0X")
                    return stringValue.substr(2);
            }
            return stringValue;
        }
    }

    template <typename NumberType>
    Status parseNumberFromStringWithBase(
            const StringData& stringValue, int base, NumberType* result) {

        typedef ::std::numeric_limits<NumberType> limits;

        if (base == 1 || base < 0 || base > 36)
            return Status(ErrorCodes::BadValue, "Invalid base", 0);

        if (stringValue.size() == 0)
            return Status(ErrorCodes::FailedToParse, "Empty string");

        bool isNegative = false;
        StringData str = _extractBase(_extractSign(stringValue, &isNegative), base, &base);

        NumberType n(0);
        if (isNegative) {
            if (limits::is_signed) {
                for (size_t i = 0; i < str.size(); ++i) {
                    NumberType digitValue = NumberType(_digitValue(str[i]));
                    if (int(digitValue) >= base)
                        return Status(ErrorCodes::FailedToParse, "Bad digit");
                    if ((NumberType(limits::min() / base) > n) ||
                        ((limits::min() - NumberType(n * base)) > -digitValue)) {

                        return Status(ErrorCodes::FailedToParse, "Underflow");
                    }

                    n *= NumberType(base);
                    n -= NumberType(digitValue);
                }
            }
            else {
                return Status(ErrorCodes::FailedToParse, "Negative value");
            }
        }
        else {
            for (size_t i = 0; i < str.size(); ++i) {
                NumberType digitValue = NumberType(_digitValue(str[i]));
                if (int(digitValue) >= base)
                    return Status(ErrorCodes::FailedToParse, "Bad digit");
                if ((NumberType(limits::max() / base) < n) ||
                    (NumberType(limits::max() - n * base) < digitValue)) {

                    return Status(ErrorCodes::FailedToParse, "Overflow");
                }

                n *= NumberType(base);
                n += NumberType(digitValue);
            }
        }
        *result = n;
        return Status::OK();
    }

    // Definition of the various supported implementations of parseNumberFromStringWithBase.

#define DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(NUMBER_TYPE)          \
    template Status parseNumberFromStringWithBase<NUMBER_TYPE>(const StringData&, int, NUMBER_TYPE*);

    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(short)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned short)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(int)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned int)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(int8_t);
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(uint8_t);

}  // namespace mongo
