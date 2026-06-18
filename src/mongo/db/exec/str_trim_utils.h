/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <array>
#include <string_view>
#include <vector>

namespace mongo::str_trim_utils {

constexpr inline size_t kMaximumAllowedTrimStringBytes = 4096;

inline const std::vector<std::string_view>& defaultTrimWhitespaceChars() {
    using namespace std::literals::string_view_literals;
    static StaticImmortal vec = std::vector<std::string_view>{
        "\0"sv,      // Null character. Avoid using "\u0000" syntax to work around a gcc
                     // bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53690.
        "\u0020"sv,  // Space
        "\u0009"sv,  // Horizontal tab
        "\u000A"sv,  // Line feed/new line
        "\u000B"sv,  // Vertical tab
        "\u000C"sv,  // Form feed
        "\u000D"sv,  // Horizontal tab
        "\u00A0"sv,  // Non-breaking space
        "\u1680"sv,  // Ogham space mark
        "\u2000"sv,  // En quad
        "\u2001"sv,  // Em quad
        "\u2002"sv,  // En space
        "\u2003"sv,  // Em space
        "\u2004"sv,  // Three-per-em space
        "\u2005"sv,  // Four-per-em space
        "\u2006"sv,  // Six-per-em space
        "\u2007"sv,  // Figure space
        "\u2008"sv,  // Punctuation space
        "\u2009"sv,  // Thin space
        "\u200A"sv,  // Hair space
    };
    return *vec;
}

/**
 * Assuming 'charByte' is the beginning of a UTF-8 code point, returns the number of bytes that
 * should be used to represent the code point. Said another way, computes how many continuation
 * bytes are expected to be present after 'charByte' in a UTF-8 encoded string.
 */
size_t numberOfBytesForCodePoint(char charByte);

/**
 * Returns a vector with one entry per code point to trim, or throws an exception if 'utf8String'
 * contains invalid UTF-8.
 */
std::vector<std::string_view> extractCodePointsFromChars(std::string_view utf8String);

bool codePointMatchesAtIndex(std::string_view input,
                             std::size_t indexOfInput,
                             std::string_view testCP);

std::string_view trimFromLeft(std::string_view input, const std::vector<std::string_view>& trimCPs);

std::string_view trimFromRight(std::string_view input,
                               const std::vector<std::string_view>& trimCPs);

std::string_view doTrim(std::string_view input,
                        const std::vector<std::string_view>& trimCPs,
                        bool trimLeft,
                        bool trimRight);

}  // namespace mongo::str_trim_utils
