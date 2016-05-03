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
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
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

    /**
     * Construct a String with UTF-8 source data (supports standard C++ string literals, and
     * std::strings).
     */
    explicit String(StringData utf8_src);

    /**
     * Reset the String with the new UTF-8 source data, reusing the underlying buffer when possible.
     */
    void resetData(const StringData utf8_src);

    /**
     * Takes a substring of the current String and puts it in another String.
     * Overwrites buffer's previous contents rather than appending.
     */
    StringData substrToBuf(StackBufBuilder* buffer, size_t pos, size_t len) const;

    /**
     * Lowercases a substring of the current String and stores the UTF8 result in buffer.
     * Overwrites buffer's previous contents rather than appending.
     */
    StringData toLowerToBuf(StackBufBuilder* buffer,
                            CaseFoldMode mode,
                            size_t offset = 0,
                            size_t len = std::string::npos) const;

    /**
     * Returns a UTF-8 encoded std::string version of the String instance. Uses the conversion
     * stored in the output buffer when possible.
     */
    std::string toString();

    /**
     * Returns the number Unicode codepoints in the String.
     */
    size_t size() const {
        return _data.size();
    }

    /**
     * Returns the Unicode codepoint at index i of the String.
     */
    const char32_t& operator[](int i) const {
        return _data[i];
    }

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
    static bool substrMatch(const std::string& str,
                            const std::string& find,
                            SubstrMatchOptions options,
                            CaseFoldMode mode = CaseFoldMode::kNormal);

    /**
     * Strips diacritics and case-folds the utf8 input string, as needed to support options.
     *
     * The options field specifies what operations to *skip*, so kCaseSensitive means to skip case
     * folding and kDiacriticSensitive means to skip diacritic striping. If both flags are
     * specified, the input utf8 StringData is returned directly without any processing or copying.
     *
     * If processing is performed, the returned StringData will be placed in buffer. buffer's
     * contents (if any) will be replaced. Since we may return the input unmodified the returned
     * StringData's lifetime is the shorter of the input utf8 and the next modification to buffer.
     * The input utf8 must not point into buffer.
     */
    static StringData caseFoldAndStripDiacritics(StackBufBuilder* buffer,
                                                 StringData utf8,
                                                 SubstrMatchOptions options,
                                                 CaseFoldMode mode);

private:
    /**
     * Helper method for converting a UTF-8 string to a UTF-32 string.
     */
    void setData(const StringData utf8_src);

    /**
     * Unified implementation of substrToBuf and toLowerToBuf.
     */
    template <typename Func>
    StringData substrToBufWithTransform(StackBufBuilder* buffer,
                                        size_t pos,
                                        size_t len,
                                        Func transform) const;

    /**
     * The underlying UTF-32 data.
     */
    std::u32string _data;

    /**
     * A buffer for storing the result of the UTF-32 to UTF-8 conversion.
     */
    std::string _outputBuf;

    /**
     * A bool flag that is set to true when toString() will require that the UTF-32 to UTF-8
     * conversion be applied again.
     */
    bool _needsOutputConversion;
};

}  // namespace unicode
}  // namespace mongo
