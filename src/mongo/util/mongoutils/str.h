// @file str.h

/*    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

/**
 * String utilities.
 *
 * TODO: De-inline.
 * TODO: Retire the mongoutils namespace, and move str under the mongo namespace.
 */

#include <sstream>
#include <string>

#include "mongo/bson/util/builder.h"

namespace mongoutils {

namespace str {

/** the idea here is to make one liners easy.  e.g.:

       return str::stream() << 1 << ' ' << 2;

    since the following doesn't work:

       (std::stringstream() << 1).str();
*/
class stream {
public:
    mongo::StringBuilder ss;
    template <class T>
    stream& operator<<(const T& v) {
        ss << v;
        return *this;
    }
    operator std::string() const {
        return ss.str();
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

inline bool equals(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

/** find char x, and return rest of std::string thereafter, or "" if not found */
inline const char* after(const char* s, char x) {
    const char* p = strchr(s, x);
    return (p != 0) ? p + 1 : "";
}
inline std::string after(const std::string& s, char x) {
    const char* p = strchr(s.c_str(), x);
    return (p != 0) ? std::string(p + 1) : "";
}

/** find std::string x, and return rest of std::string thereafter, or "" if not found */
inline const char* after(const char* s, const char* x) {
    const char* p = strstr(s, x);
    return (p != 0) ? p + strlen(x) : "";
}
inline std::string after(const std::string& s, const std::string& x) {
    const char* p = strstr(s.c_str(), x.c_str());
    return (p != 0) ? std::string(p + x.size()) : "";
}

/** @return true if s contains x
 *  These should not be used with strings containing NUL bytes
 */
inline bool contains(const std::string& s, const std::string& x) {
    return strstr(s.c_str(), x.c_str()) != 0;
}
inline bool contains(const std::string& s, char x) {
    verify(x != '\0');  // this expects c-strings so don't use when looking for NUL bytes
    return strchr(s.c_str(), x) != 0;
}

/** @return everything before the character x, else entire std::string */
inline std::string before(const std::string& s, char x) {
    const char* p = strchr(s.c_str(), x);
    return (p != 0) ? s.substr(0, p - s.c_str()) : s;
}

/** @return everything before the std::string x, else entire std::string */
inline std::string before(const std::string& s, const std::string& x) {
    const char* p = strstr(s.c_str(), x.c_str());
    return (p != 0) ? s.substr(0, p - s.c_str()) : s;
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
        if (!isdigit(*p))
            break;
        x = x * 10 + (*p - '0');
        p++;
    }
    return x;
}

/** split a std::string on a specific char.  We don't split N times, just once
    on the first occurrence.  If char not present entire std::string is in L
    and R is empty.
    @return true if char found
*/
inline bool splitOn(const std::string& s, char c, std::string& L, std::string& R) {
    const char* start = s.c_str();
    const char* p = strchr(start, c);
    if (p == 0) {
        L = s;
        R.clear();
        return false;
    }
    L = std::string(start, p - start);
    R = std::string(p + 1);
    return true;
}
/** split scanning reverse direction. Splits ONCE ONLY. */
inline bool rSplitOn(const std::string& s, char c, std::string& L, std::string& R) {
    const char* start = s.c_str();
    const char* p = strrchr(start, c);
    if (p == 0) {
        L = s;
        R.clear();
        return false;
    }
    L = std::string(start, p - start);
    R = std::string(p + 1);
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
inline std::string ltrim(const std::string& s) {
    const char* p = s.c_str();
    while (*p == ' ')
        p++;
    return p;
}

}  // namespace str

}  // namespace mongoutils

namespace mongo {
using namespace mongoutils;

#if defined(_WIN32)
inline int strcasecmp(const char* s1, const char* s2) {
    return _stricmp(s1, s2);
}
#endif

}  // namespace mongo
