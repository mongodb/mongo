// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
