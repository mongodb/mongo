// util/hex.h

/*    Copyright 2009 10gen Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {
// can't use hex namespace because it conflicts with hex iostream function
inline int fromHex(char c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    verify(false);
    return 0xff;
}
inline char fromHex(const char* c) {
    return (char)((fromHex(c[0]) << 4) | fromHex(c[1]));
}
inline char fromHex(StringData c) {
    return (char)((fromHex(c[0]) << 4) | fromHex(c[1]));
}

inline std::string toHex(const void* inRaw, int len) {
    static const char hexchars[] = "0123456789ABCDEF";

    StringBuilder out;
    const char* in = reinterpret_cast<const char*>(inRaw);
    for (int i = 0; i < len; ++i) {
        char c = in[i];
        char hi = hexchars[(c & 0xF0) >> 4];
        char lo = hexchars[(c & 0x0F)];

        out << hi << lo;
    }

    return out.str();
}

template <typename T>
std::string integerToHex(T val);

inline std::string toHexLower(const void* inRaw, int len) {
    static const char hexchars[] = "0123456789abcdef";

    StringBuilder out;
    const char* in = reinterpret_cast<const char*>(inRaw);
    for (int i = 0; i < len; ++i) {
        char c = in[i];
        char hi = hexchars[(c & 0xF0) >> 4];
        char lo = hexchars[(c & 0x0F)];

        out << hi << lo;
    }

    return out.str();
}

/* @return a dump of the buffer as hex byte ascii output */
std::string hexdump(const char* data, unsigned len);
}
