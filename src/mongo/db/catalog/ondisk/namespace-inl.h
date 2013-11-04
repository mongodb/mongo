// namespace-inl.h

/**
*    Copyright (C) 2009 10gen Inc.
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

namespace mongo {

    inline Namespace& Namespace::operator=(const StringData& ns) {
        // we fill the remaining space with all zeroes here.  as the full Namespace struct is in
        // the datafiles (the .ns files specifically), that is helpful as then they are deterministic
        // in the bytes they have for a given sequence of operations.  that makes testing and debugging
        // the data files easier.
        //
        // if profiling indicates this method is a significant bottleneck, we could have a version we
        // use for reads which does not fill with zeroes, and keep the zeroing behavior on writes.
        //
        memset( buf, 0, MaxNsLen );
        uassert( 10080 , "ns name too long, max size is 128", ns.size() < MaxNsLen - 1);
        ns.copyTo( buf, true );
        return *this;
    }

    inline string Namespace::extraName(int i) const {
        char ex[] = "$extra";
        ex[5] += i;
        string s = string(buf) + ex;
        massert( 10348 , "$extra: ns name too long", s.size() < MaxNsLen);
        return s;
    }

    inline bool Namespace::isExtra() const {
        const char *p = strstr(buf, "$extr");
        return p && p[5] && p[6] == 0; //==0 important in case an index uses name "$extra_1" for example
    }

    inline int Namespace::hash() const {
        unsigned x = 0;
        const char *p = buf;
        while ( *p ) {
            x = x * 131 + *p;
            p++;
        }
        return (x & 0x7fffffff) | 0x8000000; // must be > 0
    }

}  // namespace mongo
