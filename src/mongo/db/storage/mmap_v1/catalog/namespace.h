// namespace.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include <cstring>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {

#pragma pack(1)
/**
 * This is used for storing a namespace on disk in a fixed witdh form
 * it should only be used for that, not for passing internally
 * for that, please use NamespaceString
 */
class Namespace {
public:
    Namespace(StringData ns) {
        *this = ns;
    }
    Namespace& operator=(StringData ns);

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

    int hash() const;  // value returned is always > 0

    size_t size() const {
        return strlen(buf);
    }

    std::string toString() const {
        return buf;
    }
    operator std::string() const {
        return buf;
    }

    /* NamespaceDetails::Extra was added after fact to allow chaining of data blocks to support more
     * than 10 indexes (more than 10 IndexDetails).  It's a bit hacky because of this late addition
     * with backward file support. */
    std::string extraName(int i) const;
    /* ends with $extr... -- when true an extra block not a normal NamespaceDetails block */
    bool isExtra() const;

    enum MaxNsLenValue {
        // Maximum possible length of name any namespace, including special ones like $extra.
        // This includes rum for the NUL byte so it can be used when sizing buffers.
        MaxNsLenWithNUL = 128,

        // MaxNsLenWithNUL excluding the NUL byte. Use this when comparing std::string lengths.
        MaxNsLen = MaxNsLenWithNUL - 1,

        // Maximum allowed length of fully qualified namespace name of any real collection.
        // Does not include NUL so it can be directly compared to std::string lengths.
        MaxNsColletionLen = MaxNsLen - 7 /*strlen(".$extra")*/,
    };

private:
    char buf[MaxNsLenWithNUL];
};
#pragma pack()

}  // namespace mongo

#include "mongo/db/storage/mmap_v1/catalog/namespace-inl.h"
