// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * String utilities.
 *
 * TODO: De-inline.
 */

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/bits.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"
#include "mongo/util/str_basic.h"  // IWYU pragma: export

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#ifndef _WIN32
#include <strings.h>
#endif

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace str {

/** the idea here is to make one liners easy.  e.g.:

       return str::stream() << 1 << ' ' << 2;

    since the following doesn't work:

       (std::stringstream() << 1).str();

    TODO: To avoid implicit conversions in relational operation expressions, this stream
    class should provide a full symmetric set of relational operators vs itself, vs
    std::string, vs std::string_view, and vs const char*, but that's a lot of functions.
*/
class stream {
public:
    StringBuilder ss;
    template <class T>
    stream& operator<<(const T& v) {
        ss << v;
        return *this;
    }
    operator std::string() const {
        return ss.str();
    }
    operator std::string_view() const {
        return ss.stringData();
    }

    // Explicit ostream operator to resolve ambiguity: with std::string_view = std::string_view,
    // both operator std::string() and operator std::string_view() (= operator std::string_view())
    // would compete for operator<<(ostream, ...), causing an ambiguous overload error.
    friend std::ostream& operator<<(std::ostream& os, const stream& s) {
        return os << s.ss.stringData();
    }

    /**
     * Fail to compile if passed an unevaluated function, rather than allow it to decay and invoke
     * the bool overload. This catches both passing std::hex (which isn't supported by this type)
     * and forgetting to add () when doing `stream << someFuntion`.
     */
    template <typename R, typename... Args>
    stream& operator<<(R (*val)(Args...)) = delete;

    bool operator==(std::string_view s) const {
        return ss.stringData() == s;
    }

    auto operator<=>(std::string_view s) const {
        return ss.stringData() <=> s;
    }
};

inline bool startsWith(const char* str, const char* prefix) {
    const char* s = str;
    const char* p = prefix;
    while (*p) {
        if (*p != *s)
            return false;
        p++;
        s++;
    }
    return true;
}
inline bool startsWith(const std::string& s, const std::string& p) {
    return startsWith(s.c_str(), p.c_str());
}

// while these are trivial today use in case we do different wide char things later
inline bool startsWith(const char* p, char ch) {
    return *p == ch;
}
inline bool startsWith(const std::string& s, char ch) {
    return startsWith(s.c_str(), ch);
}

inline bool endsWith(const std::string& s, const std::string& p) {
    int l = p.size();
    int x = s.size();
    if (x < l)
        return false;
    return strncmp(s.c_str() + x - l, p.c_str(), l) == 0;
}
inline bool endsWith(const char* s, char p) {
    size_t len = strlen(s);
    return len && s[len - 1] == p;
}
inline bool endsWith(const char* p, const char* suffix) {
    size_t a = strlen(p);
    size_t b = strlen(suffix);
    if (b > a)
        return false;
    return strcmp(p + a - b, suffix) == 0;
}

/** find char x, and return rest of the string thereafter, or an empty string if not found */
inline const char* after(const char* s, char x) {
    const char* p = strchr(s, x);
    return (p != nullptr) ? p + 1 : "";
}
inline std::string_view after(std::string_view s, char x) {
    auto pos = s.find(x);
    return s.substr(pos == std::string::npos ? s.size() : pos + 1);
}

/** find string x, and return rest of the string thereafter, or an empty string if not found */
inline const char* after(const char* s, const char* x) {
    const char* p = strstr(s, x);
    return (p != nullptr) ? p + strlen(x) : "";
}
inline std::string_view after(std::string_view s, std::string_view x) {
    auto pos = s.find(x);
    return s.substr(pos == std::string::npos ? s.size() : pos + x.size());
}

/** @return true if s contains x */
inline bool contains(std::string_view s, std::string_view x) {
    return s.find(x) != std::string::npos;
}

/** @return true if s contains x */
inline bool contains(std::string_view s, char x) {
    return s.find(x) != std::string::npos;
}

/** @return everything before the character x, else entire string */
inline std::string_view before(const char* s, char x) {
    const char* p = s;
    // loop instead of strchr, so if we fail to find we don't have to iterate again.
    for (; *p && *p != x; ++p) {
    }
    return std::string_view(s, p - s);
}

/** @return everything before the character x, else entire string */
inline std::string_view before(std::string_view s, char x) {
    auto pos = s.find(x);
    return pos == std::string::npos ? s : s.substr(0, pos);
}

/** @return everything before the string x, else entire string */
inline std::string_view before(std::string_view s, std::string_view x) {
    auto pos = s.find(x);
    return pos != std::string::npos ? s.substr(0, pos) : s;
}

/** check if if strings share a common starting prefix
    @return offset of divergence (or length if equal).  0=nothing in common. */
inline int shareCommonPrefix(const char* p, const char* q) {
    int ofs = 0;
    while (1) {
        if (*p == 0 || *q == 0)
            break;
        if (*p != *q)
            break;
        p++;
        q++;
        ofs++;
    }
    return ofs;
}
inline int shareCommonPrefix(const std::string& a, const std::string& b) {
    return shareCommonPrefix(a.c_str(), b.c_str());
}

/** std::string to unsigned. zero if not a number. can end with non-num chars */
inline unsigned toUnsigned(const std::string& a) {
    unsigned x = 0;
    const char* p = a.c_str();
    while (1) {
        if (!ctype::isDigit(*p))
            break;
        x = x * 10 + (*p - '0');
        p++;
    }
    return x;
}

/** split a string on a specific char.  We don't split N times, just once on the first occurrence.
   If char not present, 'before' contains entire input string and 'after' is empty.
   @return true if char found
*/
inline bool splitOn(std::string_view s, char c, std::string_view& before, std::string_view& after) {
    auto pos = s.find(c);
    if (pos == std::string::npos) {
        before = s;
        after = std::string_view();
        return false;
    }
    before = s.substr(0, pos);
    after = s.substr(pos + 1);
    return true;
}
/** split scanning reverse direction. Splits ONCE ONLY. */
inline bool rSplitOn(std::string_view s,
                     char c,
                     std::string_view& before,
                     std::string_view& after) {
    auto pos = s.rfind(c);
    if (pos == std::string::npos) {
        before = s;
        after = std::string_view();
        return false;
    }
    before = s.substr(0, pos);
    after = s.substr(pos + 1);
    return true;
}

/** @return number of occurrences of c in s */
inline unsigned count(const std::string& s, char c) {
    unsigned n = 0;
    for (unsigned i = 0; i < s.size(); i++)
        if (s[i] == c)
            n++;
    return n;
}

/** trim leading spaces. spaces only, not tabs etc. */
inline std::string_view ltrim(std::string_view s) {
    auto i = s.data();
    auto end = s.data() + s.size();
    for (; i != end && *i == ' '; ++i) {
    }
    return s.substr(i - s.data());
}

/**
 * UTF-8 multi-byte code points consist of one leading byte of the form 11xxxxxx, and potentially
 * many continuation bytes of the form 10xxxxxx. This method checks whether 'charByte' is a leading
 * byte.
 */
inline bool isLeadingByte(char charByte) {
    return (charByte & 0xc0) == 0xc0;
}

/**
 * UTF-8 single-byte code points are of the form 0xxxxxxx. This method checks whether 'charByte' is
 * a single-byte code point.
 */
inline bool isSingleByte(char charByte) {
    return (charByte & 0x80) == 0x0;
}

inline size_t getCodePointLength(char charByte) {
    if (isSingleByte(charByte)) {
        return 1;
    }

    invariant(isLeadingByte(charByte));

    // In UTF-8, the number of leading ones is the number of bytes the code point takes up.
    return countLeadingZeros64(static_cast<unsigned char>(~charByte)) - 56;
}

/**
 * UTF-8 multi-byte code points consist of one leading byte of the form 11xxxxxx, and potentially
 * many continuation bytes of the form 10xxxxxx. This method checks whether 'charByte' is a
 * continuation byte.
 */
inline bool isUTF8ContinuationByte(char charByte) {
    return (charByte & 0xc0) == 0x80;
}

/**
 * Assuming 'str' stores a UTF-8 string, returns the number of UTF codepoints. The return value is
 * undefined if the input is not a well formed UTF-8 string.
 */
inline size_t lengthInUTF8CodePoints(std::string_view str) {
    size_t strLen = 0;
    for (char byte : str) {
        strLen += !isUTF8ContinuationByte(byte);
    }

    return strLen;
}

// Performs truncation at closest UTF-8 codepoint boundary to guarantee the end result to be valid
// UTF-8 Input encoding has to be valid UTF-8. Random-access iterator required
template <typename Iterator>
Iterator UTF8SafeTruncation(Iterator begin, Iterator end, std::size_t maximum) {
    // If we are requesting more bytes than exists in the range, then there's nothing to do
    if (static_cast<size_t>(end - begin) <= maximum)
        return end;

    const auto rbegin = std::make_reverse_iterator(begin + maximum);
    const auto rend = std::make_reverse_iterator(begin);
    auto it = rbegin;

    // Look back until we find the beginning of a unicode codepoint, extract its expected number of
    // bytes
    int codepoint_bytes = 0;
    for (; it != rend; ++it) {
        if ((*it & 0b1000'0000) == 0) {
            codepoint_bytes = 1;
            break;
        } else if ((*it & 0b1100'0000) == 0b1100'0000) {
            codepoint_bytes = 2;
            uint8_t byte = static_cast<uint8_t>(*it) << 1;
            while ((codepoint_bytes < 4) && ((byte <<= 1) & 0b1000'0000))
                ++codepoint_bytes;
            break;
        }
    }

    // Check we had the expected number of continuation bytes. If not skip this codepoint.
    int offset = codepoint_bytes - 1;
    if (std::distance(rbegin, it) != offset)
        offset = -1;  // This was a broken codepoint, go back one extra step to skip it

    return it.base() + offset;
}

inline std::string_view UTF8SafeTruncation(std::string_view input, std::size_t maximum) {
    auto truncatedEnd = UTF8SafeTruncation(input.begin(), input.end(), maximum);
    return input.substr(0, truncatedEnd - input.begin());
}

inline int caseInsensitiveCompare(const char* s1, const char* s2) {
#if defined(_WIN32)
    return _stricmp(s1, s2);
#else
    return strcasecmp(s1, s2);
#endif
}

/** Uses tolower, and therefore does not handle some languages correctly. */
constexpr bool equalCaseInsensitive(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
        std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ac, char bc) {
               return ctype::toLower(ac) == ctype::toLower(bc);
           });
}

void splitStringDelim(const std::string& str, std::vector<std::string>* res, char delim);

void joinStringDelim(const std::vector<std::string>& strs, std::string* res, char delim);

inline std::string toLower(std::string_view input) {
    std::string r{input};
    for (char& c : r)
        c = ctype::toLower(c);
    return r;
}

/** Functor for combining lexical and numeric comparisons. */
class LexNumCmp {
public:
    /** @param lexOnly - compare all characters lexically, including digits. */
    LexNumCmp(bool lexOnly);
    /**
     * Non numeric characters are compared lexicographically; numeric substrings
     * are compared numerically; dots separate ordered comparable subunits.
     * For convenience, character 255 is greater than anything else.
     * @param lexOnly - compare all characters lexically, including digits.
     */
    static int cmp(std::string_view s1, std::string_view s2, bool lexOnly);
    int cmp(std::string_view s1, std::string_view s2) const;
    bool operator()(std::string_view s1, std::string_view s2) const;

private:
    bool _lexOnly;
};

/**
 * A method to escape whitespace and control characters in strings. For example, the string "\t"
 * goes to "\\t". If `escape_slash` is true, then "/" goes to "\\/".
 */
std::string escape(std::string_view s, bool escape_slash = false);

/**
 * Converts 'integer' from a base-10 string to a size_t value or returns boost::none if 'integer'
 * is not a valid base-10 string. A valid string is not allowed to have anything but decimal
 * numerals, not even a +/- prefix or leading/trailing whitespace.
 */
boost::optional<size_t> parseUnsignedBase10Integer(std::string_view integer);

/**
 * Converts a double to a string with specified precision. If unspecified, default to 17, which is
 * the maximum decimal precision possible from a standard double.
 */
std::string convertDoubleToString(double d, int prec = 17);

}  // namespace str

inline namespace literals {
/**
 * In C++20, the "u8" prefix yields a new type, char8_t. However, many of our
 * interfaces really want to deal in plain char*. Since the u8 is about the encoding
 * not the type, provide a widget that converts the type back to a const char*. So when
 * you say:
 *
 *     const auto testString = u8"..."_as_char_ptr;
 *
 * You get back a vanilla const char* but to something with a known
 * encoding. In C++17 mode, there is nothing to do.
 */
#if defined(__cpp_char8_t) && (__cpp_char8_t >= 201811L)
inline auto operator""_as_char_ptr(const char8_t* p, std::size_t s) {
    return static_cast<const char*>(static_cast<const void*>(p));
}
#else
inline auto operator""_as_char_ptr(const char* p, std::size_t s) {
    return p;
}
#endif
}  // namespace literals

}  // namespace mongo
