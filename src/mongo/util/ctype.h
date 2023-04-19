/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
 * Replacements for <cctype> or <ctype.h> functions and macros.
 * These should be used instead of the corresponding standard functions.
 * Note the camel-case spelling to distinguish these from the C++ functions
 * and especially the C macros.
 *
 * Regarding the capitalization of these functions: POSIX defines standard
 * identifiers for the 12 character classes. Each "is"- function here directly
 * references and evokes such a POSIX identifier, so they are not
 * camel-cased as ordinary English phrases (so `isAlnum` not `isAlNum`).
 *
 * <https://en.wikipedia.org/wiki/Regular_expression#Character_classes>
 *
 * Problems with the standard functions:
 *
 *   - They accept int (to accept the EOF of -1 and integrate with cstdio).
 *     Passing negative char values other than EOF is undefined behavior!
 *     They cannot be used directly in std algorithms operating on char
 *     arguments because of this, say to `std::transform` or `std::find_if`
 *     on a `std::string`. You need a lambda and it has to do a cast.
 *   - Most are locale dependent, so they have to be slow. Dropping
 *     locale makes the "is"- functions 200% faster.
 *   - They return int instead of bool for C compatibility. Undesirable in C++.
 *   - In C they are macros, so they are very different entities depending on
 *     the subtle choice of #include <cctype> vs #include <ctype.h>.
 *   - Support for the EOF value bloats the lookup tables and carves out a
 *     surprising special case.
 *
 * The `<cctype>` character classification functions are a subtle source of bugs.
 * See warnings at <https://en.cppreference.com/w/cpp/header/cctype>.
 *
 * The proper call sequence is often not done, creating bugs. So
 * here are some more suitable C++17 implementations. We can make our versions
 * constexpr and noexcept because they don't depend on the locale or other
 * dynamic program state.
 */

#pragma once

#include <array>
#include <cstdint>

namespace mongo::ctype {
namespace detail {

/** Define a bit position for each character class queryable with this API. */
enum ClassBit : uint16_t {
    kUpper = 1 << 0,   //< [upper] UPPERCASE
    kLower = 1 << 1,   //< [lower] lowercase
    kAlpha = 1 << 2,   //< [alpha] Alphabetic (upper case or lower case)
    kDigit = 1 << 3,   //< [digit] Decimal digit
    kXdigit = 1 << 4,  //< [xdigit] Hexadecimal digit (upper case or lower case: [0-9A-Fa-f])
    kSpace = 1 << 5,   //< [space] Whitespace ([ \t\r\n\f\v])
    kPrint = 1 << 6,   //< [print] Printing (non-control chars)
    kGraph = 1 << 7,   //< [graph] Graphical (non-control, non-whitespace)
    kBlank = 1 << 8,   //< [blank] Blank (' ', '\t')
    kCntrl = 1 << 9,   //< [cntrl] Control character: 0x00-0x1f, and 0x7f (DEL)
    kPunct = 1 << 10,  //< [punct] Punctuation (graphical, but not alphanumeric)
    kAlnum = 1 << 11   //< [alnum] Alphanumeric (letter or digit)
};

/** Returns the bitwise-or of all `ClassBit` pertinent to character `c`.  */
constexpr uint16_t calculateClassBits(unsigned char c) {
    if (c >= 0x80)
        return 0;
    uint16_t r = 0;
    if (c <= 0x1f || c == 0x7f)
        r |= kCntrl;
    if (!(r & kCntrl))
        r |= kPrint;
    if (c == '\t' || c == ' ')
        r |= kBlank;
    if ((r & kBlank) || c == '\n' || c == '\v' || c == '\f' || c == '\r')
        r |= kSpace;
    if (c >= 'A' && c <= 'Z')
        r |= kUpper;
    if (c >= 'a' && c <= 'z')
        r |= kLower;
    if (c >= '0' && c <= '9')
        r |= kDigit;
    if ((r & kUpper) || (r & kLower))
        r |= kAlpha;
    if ((r & kDigit) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
        r |= kXdigit;
    if ((r & kAlpha) || (r & kDigit))
        r |= kAlnum;
    if ((r & kPrint) && !(r & kSpace))
        r |= kGraph;
    if ((r & kGraph) && !(r & kAlnum))
        r |= kPunct;
    return r;
}

/** The character class memberships for each char. */
constexpr auto chClassTable = [] {
    std::array<uint16_t, 256> arr{};
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = calculateClassBits(i);
    return arr;
}();

constexpr bool isMember(char c, uint16_t mask) {
    return chClassTable[static_cast<unsigned char>(c)] & mask;
}

/** Lookup table for `toUpper`. */
constexpr auto chUpperTable = [] {
    std::array<char, 256> arr{};
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = isMember(i, kLower) ? 'A' + (i - 'a') : i;
    return arr;
}();

/** Lookup table for `toLower`. */
constexpr auto chLowerTable = [] {
    std::array<char, 256> arr{};
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = isMember(i, kUpper) ? 'a' + (i - 'A') : i;
    return arr;
}();

}  // namespace detail


/**
 * These 12 "is"- functions exactly match the <cctype> definitions for the
 * POSIX (or C) locale. See the corresponding definitions in <cctype>.
 * <https://en.cppreference.com/w/cpp/header/cctype>
 * See notes above.
 */
constexpr bool isAlnum(char c) noexcept {
    return detail::isMember(c, detail::kAlnum);
}
constexpr bool isAlpha(char c) noexcept {
    return detail::isMember(c, detail::kAlpha);
}
constexpr bool isLower(char c) noexcept {
    return detail::isMember(c, detail::kLower);
}
constexpr bool isUpper(char c) noexcept {
    return detail::isMember(c, detail::kUpper);
}
constexpr bool isDigit(char c) noexcept {
    return detail::isMember(c, detail::kDigit);
}
constexpr bool isXdigit(char c) noexcept {
    return detail::isMember(c, detail::kXdigit);
}
constexpr bool isCntrl(char c) noexcept {
    return detail::isMember(c, detail::kCntrl);
}
constexpr bool isGraph(char c) noexcept {
    return detail::isMember(c, detail::kGraph);
}
constexpr bool isSpace(char c) noexcept {
    return detail::isMember(c, detail::kSpace);
}
constexpr bool isBlank(char c) noexcept {
    return detail::isMember(c, detail::kBlank);
}
constexpr bool isPrint(char c) noexcept {
    return detail::isMember(c, detail::kPrint);
}
constexpr bool isPunct(char c) noexcept {
    return detail::isMember(c, detail::kPunct);
}

/**
 * Returns the upper case of `c` if `c` is lower case, otherwise `c`.
 * Unlike `std::toupper`, is not affected by locale. See notes above.
 */
constexpr char toUpper(char c) noexcept {
    return detail::chUpperTable[static_cast<unsigned char>(c)];
}

/**
 * Returns the lower case of `c` if `c` is upper case, otherwise `c`.
 * Unlike `std::tolower`, is not affected by locale. See notes above.
 */
constexpr char toLower(char c) noexcept {
    return detail::chLowerTable[static_cast<unsigned char>(c)];
}

}  // namespace mongo::ctype
