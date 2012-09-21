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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "mongo/platform/strtoll.h"

/**
 * Macro providing the specialized implementation of parseNumberFromStringWithBase<NUMBER_TYPE>
 * in terms of the strtol-like function CONV_FUNC.
 */
#define DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(NUMBER_TYPE, CONV_FUNC) \
    template<>                                                          \
    Status parseNumberFromStringWithBase<NUMBER_TYPE>(                  \
            const char* stringValue,                                    \
            int base,                                                   \
            NUMBER_TYPE* out) {                                         \
                                                                        \
        typedef ::std::numeric_limits<NUMBER_TYPE> limits;              \
                                                                        \
        if (base == 1 || base < 0 || base > 36)                         \
            return Status(ErrorCodes::BadValue, "Invalid base", 0);     \
                                                                        \
        if (stringValue[0] == '\0' || ::isspace(stringValue[0]))        \
            return Status(ErrorCodes::FailedToParse, "Unparseable string", 0); \
                                                                        \
        if (!limits::is_signed && (stringValue[0] == '-'))              \
            return Status(ErrorCodes::FailedToParse, "Negative value", 0);   \
                                                                        \
        errno = 0;                                                      \
        char* endPtr;                                                   \
        NUMBER_TYPE result = CONV_FUNC(stringValue, &endPtr, base);     \
        if (*endPtr != '\0')                                            \
            return Status(ErrorCodes::FailedToParse, "Non-digit characters at end of string");\
                                                                        \
        if ((ERANGE == errno) && ((result == limits::max()) || (result == limits::min()))) \
            return Status(ErrorCodes::FailedToParse, "Value out of range", 0); \
                                                                        \
        if (errno != 0 && result == 0)                                  \
            return Status(ErrorCodes::FailedToParse, "Unparseable string", 0); \
                                                                        \
        *out = result;                                                  \
        return Status::OK();                                            \
    }

/**
 * Macro providing the specialized implementation of parseNumberFromStringWithBase<NUMBER_TYPE>
 * in terms of parseNumberFromStringWithBase<BIGGER_NUMBER_TYPE>.
 */
#define DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE_IN_TERMS_OF(NUMBER_TYPE, BIGGER_NUMBER_TYPE) \
    template<>                                                          \
    Status parseNumberFromStringWithBase<NUMBER_TYPE>(                  \
            const char* stringValue,                                    \
            int base,                                                   \
            NUMBER_TYPE* out) {                                         \
                                                                        \
        return parseNumberFromStringWithBaseUsingBiggerNumberType<NUMBER_TYPE, \
                                                                  BIGGER_NUMBER_TYPE>( \
                stringValue, base, out);                                \
    }

namespace mongo {

namespace {

    /**
     * This function implements the functionality of parseNumberFromStringWithBase<NumberType>
     * by calling parseNumberFromStringWithBase<BiggerNumberType> and checking for overflow on
     * the range of NumberType.  Used for "int" and "short" types, for which an equivalent to
     * strtol is unavailable.
     */
    template<typename NumberType, typename BiggerNumberType>
    Status parseNumberFromStringWithBaseUsingBiggerNumberType(
            const char* stringValue,
            int base,
            NumberType* out) {
        typedef ::std::numeric_limits<NumberType> limits;
        BiggerNumberType result;
        Status status = parseNumberFromStringWithBase(stringValue, base, &result);
        if (Status::OK() != status)
            return status;
        if ((result < limits::min()) || (result > limits::max()))
            return Status(ErrorCodes::FailedToParse, "Value out of range", 0);
        *out = static_cast<NumberType>(result);
        return Status::OK();
    }
}  // namespace

    // Definition of the various supported implementations of parseNumberFromStringWithBase.

    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long, strtol)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(long long, strtoll)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long, strtoul)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE(unsigned long long, strtoull)

    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE_IN_TERMS_OF(short, long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE_IN_TERMS_OF(unsigned short, unsigned long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE_IN_TERMS_OF(int, long)
    DEFINE_PARSE_NUMBER_FROM_STRING_WITH_BASE_IN_TERMS_OF(unsigned int, unsigned long)
}  // namespace mongo
