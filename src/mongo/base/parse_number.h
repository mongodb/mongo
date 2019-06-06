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

/**
 * Utility functions for parsing numbers from strings.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/decimal128.h"

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
     * If endPtr is not nullptr, the end of the number portion of the string will be stored at
     * *endPtr (like strtol).
     * This will return with Status::FailedToParse if the string does not represent a number value.
     * See skipWhitespace and allowTrailingText for ways to expand the parser's capabilities.
     * Returns with Status::Overflow if the parsed number cannot be represented by the desired type.
     * If the status is not OK, then there are no guarantees about what value will be stored in
     * result.
     */
    Status operator()(StringData strData, long* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, long long* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, unsigned long* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData,
                      unsigned long long* result,
                      char** endPtr = nullptr) const;
    Status operator()(StringData strData, short* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, unsigned short* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, int* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, unsigned int* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, int8_t* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, uint8_t* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, double* result, char** endPtr = nullptr) const;
    Status operator()(StringData strData, Decimal128* result, char** endPtr = nullptr) const;

    int _base = 0;
    Decimal128::RoundingMode _roundingMode = Decimal128::RoundingMode::kRoundTowardZero;
    bool _skipLeadingWhitespace = false;
    bool _allowTrailingText = false;
};

}  // namespace mongo
