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
        Namespace(const StringData& ns) { *this = ns; }
        Namespace& operator=(const StringData& ns);

        void kill() { buf[0] = 0x7f; }

        bool operator==(const char *r) const { return strcmp(buf, r) == 0; }
        bool operator==(const Namespace& r) const { return strcmp(buf, r.buf) == 0; }
        bool operator!=(const char *r) const { return strcmp(buf, r) != 0; }
        bool operator!=(const Namespace& r) const { return strcmp(buf, r.buf) != 0; }

        bool hasDollarSign() const { return strchr( buf , '$' ) > 0;  }

        int hash() const; // value returned is always > 0

        size_t size() const { return strlen( buf ); }

        std::string toString() const { return buf; }
        operator std::string() const { return buf; }

        /* NamespaceDetails::Extra was added after fact to allow chaining of data blocks to support more than 10 indexes
           (more than 10 IndexDetails).  It's a bit hacky because of this late addition with backward
           file support. */
        std::string extraName(int i) const;
        bool isExtra() const; /* ends with $extr... -- when true an extra block not a normal NamespaceDetails block */

        enum MaxNsLenValue { MaxNsLen = 128 };
    private:
        char buf[MaxNsLen];
    };
#pragma pack()

} // namespace mongo

#include "mongo/db/storage/namespace-inl.h"
