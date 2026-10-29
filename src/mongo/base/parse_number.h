// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Utility functions for parsing numbers from strings.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Builder pattern for setting up a number parser. Intended usage:
 *     long result;
 *     char* end;
 *     NumberParser()
 *     .base(16)
 *     .allowTrailingText()
 *     .skipWhitespace()
 *     ("\t\n    0x16hello, world", &result, &end);
 *     //end points to 'h' and result holds 22
 */
struct NumberParser {
public:
    /**
     * Behave like strtol/atoi and skip whitespace at the beginning of the string
     */
    NumberParser& skipWhitespace(bool skipws = true) {
        _skipLeadingWhitespace = skipws;
        return *this;
    }

    /**
     * Set a base for the conversion. 0 means infer the base akin to strtol.
     * Legal bases are [2-35]. If a base outside of this is selected, then operator()
     * will return BadValue.
     */
    NumberParser& base(int b = 0) {
        _base = b;
        return *this;
    }

    /*
     * Acts like atoi/strtol and will still parse even if there are non-numeric characters in the
     * string after the number. Without this option, the parser will return FailedToParse if there
     * are leftover characters in the parsed string.
     */
    NumberParser& allowTrailingText(bool allowTrailingText = true) {
        _allowTrailingText = allowTrailingText;
        return *this;
    }

    NumberParser& setDecimal128RoundingMode(
        Decimal128::RoundingMode mode = Decimal128::RoundingMode::kRoundTiesToEven) {
        _roundingMode = mode;
        return *this;
    }

    /*
     * returns a NumberParser configured like strtol/atoi
     */
    static NumberParser strToAny(int base = 0) {
        return NumberParser().skipWhitespace().base(base).allowTrailingText();
    }

    /*
     * Parsing overloads for different supported numerical types.
     *
     * On success, the parsed value is stored into *result and returns Status::OK().
     * If end is not nullptr, the end of the number portion of the string will be stored at
     * *end (like strtol).
     * This will return with Status::FailedToParse if the string does not represent a number value.
     * See skipWhitespace and allowTrailingText for ways to expand the parser's capabilities.
     * Returns with Status::Overflow if the parsed number cannot be represented by the desired type.
     * If the status is not OK, then there are no guarantees about what value will be stored in
     * result.
     */
    Status operator()(std::string_view s, long* out, const char** end = {}) const;
    Status operator()(std::string_view s, long long* out, const char** end = {}) const;
    Status operator()(std::string_view s, unsigned long* out, const char** end = {}) const;
    Status operator()(std::string_view s, unsigned long long* out, const char** end = {}) const;
    Status operator()(std::string_view s, short* out, const char** end = {}) const;
    Status operator()(std::string_view s, unsigned short* out, const char** end = {}) const;
    Status operator()(std::string_view s, int* out, const char** end = {}) const;
    Status operator()(std::string_view s, unsigned int* out, const char** end = {}) const;
    Status operator()(std::string_view s, int8_t* out, const char** end = {}) const;
    Status operator()(std::string_view s, uint8_t* out, const char** end = {}) const;
    Status operator()(std::string_view s, double* out, const char** end = {}) const;
    Status operator()(std::string_view s, Decimal128* out, const char** end = {}) const;

    int _base = 0;
    Decimal128::RoundingMode _roundingMode = Decimal128::RoundingMode::kRoundTowardZero;
    bool _skipLeadingWhitespace = false;
    bool _allowTrailingText = false;
};

}  // namespace mongo
