/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstdint>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/fts/unicode/codepoints.h"

namespace mongo {
namespace unicode {

/**
 * A string class that support basic Unicode functionality such as removing diacritic marks, and
 * lowercasing. The String is constructed with UTF-8 source data, and is converted under the hood to
 * a u32string (UTF-32) so operations can be easily done with individual Unicode code points.
 */
class String {
public:
    String() = default;

#if defined(_MSC_VER) && _MSC_VER < 1900
    String(String&& other) : _data(std::move(other._data)) {}

    String& operator=(String&& other) {
        _data = std::move(other._data);
        return *this;
    }
#endif

    /**
     * Construct a String with UTF-8 source data (supports standard C++ string literals, and
     * std::strings).
     */
    explicit String(StringData utf8_src);

    /**
     * Return a lowercased version of the String instance using the Unicode data in u_data.h.
     */
    String toLower(CaseFoldMode mode = CaseFoldMode::kNormal) const;

    /**
     * Returns a version of the String instance with diacritics and combining marks removed.
     */
    String removeDiacritics() const;

    /**
     * Returns a substring of the String instance, using the same semantics as std::string::substr.
     */
    String substr(size_t begin, size_t end) const;

    /**
     * Returns a UTF-8 encoded std::string version of the String instance.
     */
    std::string toString() const;

    /**
     * Returns the number Unicode codepoints in the String.
     */
    size_t size() const;

    /**
     * Returns the Unicode codepoint at index i of the String.
     */
    const char32_t& operator[](int i) const;

    /**
     * Options for the substrMatch method.
     */
    using SubstrMatchOptions = uint8_t;

    /**
     * No options (case insensitive and diacritic insensitive).
     */
    static const SubstrMatchOptions kNone = 0;

    /**
     * Perform case sensitive substring match.
     */
    static const SubstrMatchOptions kCaseSensitive = 1 << 0;

    /**
     * Perform diacritic sensitive substring match.
     */
    static const SubstrMatchOptions kDiacriticSensitive = 1 << 1;

    /**
     * Search the string 'str' for the string 'find'. If 'find' exists in 'str', return true, else
     * return false. Optionally searches can be made case sensitive and diacritic insensitive. If
     * the search is case insensitive, non-Turkish case folding is used unless the
     * CaseFoldMode::Turkish is passed to mode.
     */
    static bool substrMatch(const String& str,
                            const String& find,
                            SubstrMatchOptions options,
                            CaseFoldMode mode = CaseFoldMode::kNormal);

private:
    /**
     * Private constructor used by substr, toLower, and removeDiacritics to build a String from
     * UTF-32 data.
     */
    String(std::u32string&& src);

    /**
     * The underlying UTF-32 data.
     */
    std::u32string _data;
};

}  // namespace unicode
}  // namespace mongo
