/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <boost/filesystem/path.hpp>
#include <cstring>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

#pragma pack(1)
/**
 * This is used for storing a namespace on disk in a fixed witdh form and should only be used for
 * that, not for passing internally.
 *
 * If you need to pass internally, please use NamespaceString.
 */
class Namespace {
public:
    Namespace(StringData ns) {
        *this = ns;
    }

    Namespace& operator=(StringData ns) {
        // We fill the remaining space with all zeroes here. As the full Namespace struct is in the
        // datafiles (the .ns files specifically), that is helpful as then they are deterministic in
        // the bytes they have for a given sequence of operations. This makes testing and debugging
        // the data files easier.
        //
        // If profiling indicates this method is a significant bottleneck, we could have a version
        // we use for reads which does not fill with zeroes, and keep the zeroing behavior on
        // writes.
        memset(buf, 0, sizeof(buf));
        uassert(10080,
                str::stream() << "ns name " << ns << " (size: " << ns.size()
                              << ") too long, max size is 127 bytes",
                ns.size() <= MaxNsLen);
        uassert(
            17380, "ns name can't contain embedded '\0' byte", ns.find('\0') == std::string::npos);
        ns.copyTo(buf, true);
        return *this;
    }

    void kill() {
        buf[0] = 0x7f;
    }

    bool operator==(const char* r) const {
        return strcmp(buf, r) == 0;
    }
    bool operator==(const Namespace& r) const {
        return strcmp(buf, r.buf) == 0;
    }
    bool operator!=(const char* r) const {
        return strcmp(buf, r) != 0;
    }
    bool operator!=(const Namespace& r) const {
        return strcmp(buf, r.buf) != 0;
    }

    bool hasDollarSign() const {
        return strchr(buf, '$') != NULL;
    }

    /**
     * Value returned is always > 0
     */
    int hash() const {
        unsigned x = 0;
        const char* p = buf;
        while (*p) {
            x = x * 131 + *p;
            p++;
        }
        return (x & 0x7fffffff) | 0x8000000;  // must be > 0
    }

    size_t size() const {
        return strlen(buf);
    }

    std::string toString() const {
        return buf;
    }
    operator std::string() const {
        return buf;
    }

    /**
     * NamespaceDetails::Extra was added after fact to allow chaining of data blocks to support more
     * than 10 indexes (more than 10 IndexDetails). It's a bit hacky because of this late addition
     * with backward file support.
     */
    std::string extraName(int i) const {
        char ex[] = "$extra";
        ex[5] += i;
        const std::string s = std::string(buf) + ex;
        massert(10348, "$extra: ns name too long", s.size() <= MaxNsLen);
        return s;
    }

    /**
     * Returns whether the namespace ends with "$extr...". When true it represents an extra block
     * not a normal NamespaceDetails block.
     */
    bool isExtra() const {
        const char* p = strstr(buf, "$extr");
        return p && p[5] &&
            p[6] == 0;  //== 0 is important in case an index uses name "$extra_1" for example
    }

    enum MaxNsLenValue {
        // Maximum possible length of name any namespace, including special ones like $extra. This
        // includes room for the NUL byte so it can be used when sizing buffers.
        MaxNsLenWithNUL = 128,

        // MaxNsLenWithNUL excluding the NUL byte. Use this when comparing std::string lengths.
        MaxNsLen = MaxNsLenWithNUL - 1,

        // Maximum allowed length of fully qualified namespace name of any real collection. Does not
        // include NUL so it can be directly compared to std::string lengths.
        MaxNsCollectionLen = MaxNsLen - 7 /*strlen(".$extra")*/,
    };

private:
    char buf[MaxNsLenWithNUL];
};
#pragma pack()

namespace {
MONGO_STATIC_ASSERT(sizeof(Namespace) == 128);
MONGO_STATIC_ASSERT(Namespace::MaxNsLenWithNUL == MaxDatabaseNameLen);
MONGO_STATIC_ASSERT((int)Namespace::MaxNsLenWithNUL == (int)NamespaceString::MaxNsLenWithNUL);
MONGO_STATIC_ASSERT((int)Namespace::MaxNsLen == (int)NamespaceString::MaxNsLen);
MONGO_STATIC_ASSERT((int)Namespace::MaxNsCollectionLen == (int)NamespaceString::MaxNsCollectionLen);
}  // namespace
}  // namespace mongo
