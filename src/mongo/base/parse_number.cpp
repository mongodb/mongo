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

#include "mongo/platform/basic.h"

#include "mongo/base/parse_number.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

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
     * Returns stringValue, unless it sets *outputBase to 16, in which case it will strip off the
     * "0x" or "0X" prefix, if present.
     */
    static inline StringData _extractBase(
            const StringData& stringValue, int inputBase, int* outputBase) {

        const StringData hexPrefixLower("0x", StringData::LiteralTag());
        const StringData hexPrefixUpper("0X", StringData::LiteralTag());
        if (inputBase == 0) {
            if (stringValue.size() > 2 && (stringValue.startsWith(hexPrefixLower) ||
                                           stringValue.startsWith(hexPrefixUpper))) {
                *outputBase = 16;
                return stringValue.substr(2);
            }
            if (stringValue.size() > 1 && stringValue[0] == '0') {
                *outputBase = 8;
                return stringValue;
            }
            *outputBase = 10;
            return stringValue;
        }
        else {
            *outputBase = inputBase;
            if (inputBase == 16 && (stringValue.startsWith(hexPrefixLower) ||
                                    stringValue.startsWith(hexPrefixUpper))) {
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

        bool isNegative = false;
        StringData str = _extractBase(_extractSign(stringValue, &isNegative), base, &base);

        if (str.empty())
            return Status(ErrorCodes::FailedToParse, "No digits");

        NumberType n(0);
        if (isNegative) {
            if (limits::is_signed) {
                for (size_t i = 0; i < str.size(); ++i) {
                    NumberType digitValue = NumberType(_digitValue(str[i]));
                    if (int(digitValue) >= base)
                        return Status(ErrorCodes::FailedToParse, "Bad digit");

// MSVC: warning C4146: unary minus operator applied to unsigned type, result still unsigned
// This code is statically known to be dead when NumberType is unsigned, so the warning is not real
#pragma warning(push)
#pragma warning(disable:4146)
                    if ((NumberType(limits::min() / base) > n) ||
                        ((limits::min() - NumberType(n * base)) > -digitValue)) {
#pragma warning(pop)

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
#undef DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE

#ifdef _WIN32

namespace {

    /**
     * Converts ascii c-locale uppercase characters to lower case, leaves other char values
     * unchanged.
     */
    char toLowerAscii(char c) {
        if (isascii(c) && isupper(c))
            return _tolower(c);
        return c;
    }

}  // namespace

#endif  // defined(_WIN32)

    template <>
    Status parseNumberFromStringWithBase<double>(const StringData& stringValue,
                                                 int base,
                                                 double* result) {
        if (base != 0) {
            return Status(ErrorCodes::BadValue,
                          "Must pass 0 as base to parseNumberFromStringWithBase<double>.");
        }
        if (stringValue.empty())
            return Status(ErrorCodes::FailedToParse, "Empty string");

        if (isspace(stringValue[0]))
            return Status(ErrorCodes::FailedToParse, "Leading whitespace");

        std::string str = stringValue.toString();
        const char* cStr = str.c_str();
        char* endp;
        errno = 0;
        double d = strtod(cStr, &endp);
        int actualErrno = errno;
        if (endp != stringValue.size() + cStr) {
#ifdef _WIN32
            // The Windows libc implementation of strtod cannot parse +/-infinity or nan,
            // so handle that here.
            std::transform(str.begin(), str.end(), str.begin(), toLowerAscii);
            if (str == StringData("nan", StringData::LiteralTag())) {
                *result = std::numeric_limits<double>::quiet_NaN();
                return Status::OK();
            }
            else if (str == StringData("+infinity", StringData::LiteralTag()) ||
                     str == StringData("infinity", StringData::LiteralTag())) {
                *result = std::numeric_limits<double>::infinity();
                return Status::OK();
            }
            else if (str == StringData("-infinity", StringData::LiteralTag())) {
                *result = -std::numeric_limits<double>::infinity();
                return Status::OK();
            }
#endif  // defined(_WIN32)

            return Status(ErrorCodes::FailedToParse, "Did not consume whole number.");
        }
        if (actualErrno == ERANGE)
            return Status(ErrorCodes::FailedToParse, "Out of range");
        *result = d;
        return Status::OK();
    }

}  // namespace mongo
