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

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

namespace mongo::str_trim_utils {

const std::vector<StringData> kDefaultTrimWhitespaceChars = {
    "\0"_sd,      // Null character. Avoid using "\u0000" syntax to work around a gcc bug:
                  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53690.
    "\u0020"_sd,  // Space
    "\u0009"_sd,  // Horizontal tab
    "\u000A"_sd,  // Line feed/new line
    "\u000B"_sd,  // Vertical tab
    "\u000C"_sd,  // Form feed
    "\u000D"_sd,  // Horizontal tab
    "\u00A0"_sd,  // Non-breaking space
    "\u1680"_sd,  // Ogham space mark
    "\u2000"_sd,  // En quad
    "\u2001"_sd,  // Em quad
    "\u2002"_sd,  // En space
    "\u2003"_sd,  // Em space
    "\u2004"_sd,  // Three-per-em space
    "\u2005"_sd,  // Four-per-em space
    "\u2006"_sd,  // Six-per-em space
    "\u2007"_sd,  // Figure space
    "\u2008"_sd,  // Punctuation space
    "\u2009"_sd,  // Thin space
    "\u200A"_sd   // Hair space
};

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
std::vector<StringData> extractCodePointsFromChars(StringData utf8String);

bool codePointMatchesAtIndex(StringData input, std::size_t indexOfInput, StringData testCP);

StringData trimFromLeft(StringData input, const std::vector<StringData>& trimCPs);

StringData trimFromRight(StringData input, const std::vector<StringData>& trimCPs);

StringData doTrim(StringData input,
                  const std::vector<StringData>& trimCPs,
                  bool trimLeft,
                  bool trimRight);

}  // namespace mongo::str_trim_utils
