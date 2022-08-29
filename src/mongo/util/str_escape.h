/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"

#include <fmt/format.h>
#include <string>

namespace mongo::str {

/**
 * Escapes the special characters in 'str' for use as printable text.
 *
 * The backslash (`\`) character is escaped with another backslash, yielding the
 * 2-character sequence {`\`, `\`}.
 *
 * The single-byte control characters (octets 0x00-0x1f, 0x7f) are generally escaped
 * using the format "\xHH", where the 2 `H` characters are replaced by the 2 hex digits
 * of the octet. For instance, the octet 0x7f would yield the sequence: {`\`, `x`, `7`, `f`}.
 * Exemptions to this rule are the following octets, which are escaped using C-style escape
 * sequences:
 *   0x00  ->  {`\`, `0`}
 *   0x07  ->  {`\`, `a`}
 *   0x08  ->  {`\`, `b`}
 *   0x09  ->  {`\`, `t`}
 *   0x0a  ->  {`\`, `n`}
 *   0x0b  ->  {`\`, `v`}
 *   0x0c  ->  {`\`, `f`}
 *   0x0d  ->  {`\`, `r`}
 *   0x1b  ->  {`\`, `e`}
 *
 * The two-byte UTF-8 sequences between 0xC280 (U+0080) and 0xC29F (U+009F), inclusive, are
 * also escaped as they are considered control characters. The escape sequence for these has
 * the format: "\xC2\xHH", where the 2 `H` characters are replaced by the 2 hex digits of the
 * second octet.
 *
 * Invalid bytes found are replaced with the escape sequence following the format: "\xHH",
 * similar to how single-byte control characters are escaped.
 *
 * This writes the escaped output to 'buffer', and stops writing when either the output
 * length reaches the 'maxLength', or if appending the next escape sequence will cause the
 * output to exceed 'maxLength'. A 'maxLength' value of std::string::npos means unbounded.
 *
 * The 'wouldWrite' output is updated to contain the total bytes that would have been written
 * if there was no length limit.
 */
void escapeForText(fmt::memory_buffer& buffer,
                   StringData str,
                   size_t maxLength = std::string::npos,
                   size_t* wouldWrite = nullptr);
std::string escapeForText(StringData str,
                          size_t maxLength = std::string::npos,
                          size_t* wouldWrite = nullptr);

/**
 * Escapes the special characters in 'str' for use in JSON.
 *
 * This differs from escapeForText in that the double-quote character (`"`) is escaped
 * with a backslash, yielding the 2-character sequence {`\`, `"`}.
 *
 * The general format of the escape sequences for single-byte control characters becomes
 * "\u00HH", where the 2 `H` characters are replaced by the 2 hex digits of the octet.
 * For example, the octet 0x7f would yield the sequence: {`\`, `u`, `0`, `0`, `7`, `f`}.
 * The list of octets escaped using C-style escape sequences is also shortened to:
 *   0x08  ->  {`\`, `b`}
 *   0x09  ->  {`\`, `t`}
 *   0x0a  ->  {`\`, `n`}
 *   0x0c  ->  {`\`, `f`}
 *   0x0d  ->  {`\`, `r`}
 * For two-byte control characters, the format of the escape sequence becomes "\uc2HH",
 * where the 2 `H` characters are replaced by the 2 hex digits of the second octet.
 * Invalid bytes found are replaced with the sequence: "\ufffd".
 */
void escapeForJSON(fmt::memory_buffer& buffer,
                   StringData str,
                   size_t maxLength = std::string::npos,
                   size_t* wouldWrite = nullptr);
std::string escapeForJSON(StringData str,
                          size_t maxLength = std::string::npos,
                          size_t* wouldWrite = nullptr);

/**
 * Returns whether a string consists with valid UTF-8 encoded characters.
 */
bool validUTF8(StringData str);
}  // namespace mongo::str
