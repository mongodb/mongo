/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/analyze_regex.h"

#include "mongo/base/string_data.h"
#include "mongo/util/ctype.h"
#include "mongo/util/str.h"

#include <cstring>

namespace mongo::analyze_regex {

namespace {
/**
 * Returns true if 'str' contains a non-escaped pipe character '|' on a best-effort basis. This
 * function reports no false negatives, but will return false positives. For example, a pipe
 * character inside of a character class or the \Q...\E escape sequence has no special meaning but
 * may still be reported by this function as being non-escaped.
 */
bool stringMayHaveUnescapedPipe(StringData str) {
    if (str.size() > 0 && str[0] == '|') {
        return true;
    }
    if (str.size() > 1 && str[1] == '|' && str[0] != '\\') {
        return true;
    }

    for (size_t i = 2U; i < str.size(); ++i) {
        auto probe = str[i];
        auto prev = str[i - 1];
        auto tail = str[i - 2];

        // We consider the pipe to have a special meaning if it is not preceded by a backslash, or
        // preceded by a backslash that is itself escaped.
        if (probe == '|' && (prev != '\\' || (prev == '\\' && tail == '\\'))) {
            return true;
        }
    }
    return false;
}
}  // namespace

std::pair<std::string, bool> getRegexPrefixMatch(const char* regex, const char* flags) {
    bool multilineOK;
    if (regex[0] == '\\' && regex[1] == 'A') {
        multilineOK = true;
        regex += 2;
    } else if (regex[0] == '^') {
        multilineOK = false;
        regex += 1;
    } else {
        return {"", false};
    }

    // A regex with an unescaped pipe character is not considered a simple regex.
    if (stringMayHaveUnescapedPipe(StringData(regex))) {
        return {"", false};
    }

    bool extended = false;
    while (*flags) {
        switch (*(flags++)) {
            case 'm':
                // Multiline mode.
                if (multilineOK)
                    continue;
                else
                    return {"", false};
            case 's':
                // Single-line mode specified. This just changes the behavior of the '.' character
                // to match every character instead of every character except '\n'.
                continue;
            case 'x':
                // Extended free-spacing mode.
                extended = true;
                break;
            default:
                // Cannot use the index.
                return {"", false};
        }
    }

    str::stream ss;

    std::string r = "";
    while (*regex) {
        char c = *(regex++);

        if (c == '*' || c == '?') {
            // These are the only two symbols that make the last char optional.
            r = ss;
            r = r.substr(0, r.size() - 1);
            // Patterns like /^a?/ and /^a*/ can be implemented by scanning all the strings
            // beginning with the prefix "a", but the regex must be reapplied. We cannot convert
            // such regexes into exact bounds.
            return {r, false};
        } else if (c == '\\') {
            c = *(regex++);
            if (c == 'Q') {
                // \Q...\E quotes everything inside.
                while (*regex) {
                    c = (*regex++);
                    if (c == '\\' && (*regex == 'E')) {
                        regex++;  // skip the 'E'
                        break;    // go back to start of outer loop
                    } else {
                        ss << c;  // character should match itself
                    }
                }
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       (c == '\0')) {
                // Bail out on any of these escape sequences.
                r = ss;
                break;
            } else {
                // Slash followed by non-alphanumeric represents the following char.
                ss << c;
            }
        } else if (strchr("^$.[()+{", c)) {
            // List of "metacharacters" from man pcrepattern.
            //
            // For prefix patterns ending in '.*' (ex. /^abc.*/) we can build exact index bounds.
            if (!multilineOK && (c == '.')) {
                c = *(regex++);
                if (c == '*' && *regex == 0) {
                    return {ss, true};
                } else {
                    c = *(regex--);
                }
            }
            r = ss;
            break;
        } else if (extended && c == '#') {
            // Comment.
            r = ss;
            break;
        } else if (extended && ctype::isSpace(c)) {
            continue;
        } else {
            // Self-matching char.
            ss << c;
        }
    }

    bool isExactPrefixMatch = false;
    if (r.empty() && *regex == 0) {
        r = ss;
        isExactPrefixMatch = !r.empty();
    }

    return {r, isExactPrefixMatch};
}

}  // namespace mongo::analyze_regex
