/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

namespace mongo {
    /** a simple, rather dumb, but very fast checksum.  see perftests.cpp for unit tests. */
    struct Checksum { 
        union { 
            unsigned char bytes[16];
            unsigned long long words[2];
        };

        // if you change this you must bump dur::CurrentVersion
        void gen(const void *buf, unsigned len) {
            wassert( ((size_t)buf) % 8 == 0 ); // performance warning
            unsigned n = len / 8 / 2;
            const unsigned long long *p = (const unsigned long long *) buf;
            unsigned long long a = 0;
            for( unsigned i = 0; i < n; i++ ) {
                a += (*p ^ i);
                p++;
            }
            unsigned long long b = 0;
            for( unsigned i = 0; i < n; i++ ) {
                b += (*p ^ i);
                p++;
            }
            unsigned long long c = 0;
            for( unsigned i = n * 2 * 8; i < len; i++ ) { // 0-7 bytes left
                c = (c << 8) | ((const char *)buf)[i];
            }
            words[0] = a ^ len;
            words[1] = b ^ c;
        }

        bool operator==(const Checksum& rhs) const { return words[0]==rhs.words[0] && words[1]==rhs.words[1]; }
        bool operator!=(const Checksum& rhs) const { return words[0]!=rhs.words[0] || words[1]!=rhs.words[1]; }
    };
}
