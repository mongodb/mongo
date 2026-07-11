// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/fts/unicode/codepoints.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mongo {
namespace unicode {

/**
 * A string class that support basic Unicode functionality such as removing diacritic marks, and
 * lowercasing. The String is constructed with UTF-8 source data, and is converted under the hood to
 * a u32string (UTF-32) so operations can be easily done with individual Unicode code points.
 *
 * Need for replacement: It's being used in fle_crud_test.cpp in a single unit test, and the only
 * member being called are the constructor, .size() and .substrToBuf().
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] String {
public:
    String() = default;

    /**
     * Construct a String with UTF-8 source data (supports standard C++ string literals, and
     * std::strings).
     */
    explicit String(std::string_view utf8_src);

    /**
     * Reset the String with the new UTF-8 source data, reusing the underlying buffer when possible.
     */
    void resetData(std::string_view utf8_src);

    /**
     * Takes a substring of the current String and puts it in another String.
     * Overwrites buffer's previous contents rather than appending.
     */
    std::string_view substrToBuf(StackBufBuilder* buffer, size_t pos, size_t len) const;

    /**
     * Lowercases a substring of the current String and stores the UTF8 result in buffer.
     * Overwrites buffer's previous contents rather than appending.
     */
    std::string_view toLowerToBuf(StackBufBuilder* buffer,
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
    static bool substrMatch(std::string_view str,
                            std::string_view find,
                            SubstrMatchOptions options,
                            CaseFoldMode mode = CaseFoldMode::kNormal);

    /**
     * Strips diacritics and case-folds the utf8 input string, as needed to support options.
     *
     * The options field specifies what operations to *skip*, so kCaseSensitive means to skip case
     * folding and kDiacriticSensitive means to skip diacritic striping. If both flags are
     * specified, the input utf8 std::string_view is returned directly without any processing or
     * copying.
     *
     * If processing is performed, the returned std::string_view will be placed in buffer. buffer's
     * contents (if any) will be replaced. Since we may return the input unmodified the returned
     * std::string_view's lifetime is the shorter of the input utf8 and the next modification to
     * buffer. The input utf8 must not point into buffer.
     */
    static std::string_view caseFoldAndStripDiacritics(StackBufBuilder* buffer,
                                                       std::string_view utf8,
                                                       SubstrMatchOptions options,
                                                       CaseFoldMode mode);

private:
    /**
     * Helper method for converting a UTF-8 string to a UTF-32 string.
     */
    void setData(std::string_view utf8_src);

    /**
     * Unified implementation of substrToBuf and toLowerToBuf.
     */
    template <typename Func>
    std::string_view substrToBufWithTransform(StackBufBuilder* buffer,
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
