/** @file hash.h */

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

namespace mongoutils {

    /** @return hash of a pointer to an unsigned. so you get a 32 bit hash out, regardless of whether
                pointers are 32 or 64 bit on the particular platform.

        is there a faster way to impl this that hashes just as well?
    */
    inline unsigned hashPointer(void *v) {
        unsigned x = 0;
        unsigned char *p = (unsigned char *) &v;
        for( unsigned i = 0; i < sizeof(void*); i++ ) {
            x = x * 131 + p[i];
        }
        return x;
    }

    inline unsigned hash(unsigned u) {
        unsigned char *p = (unsigned char *) &u;
        return (((((p[3] * 131) + p[2]) * 131) + p[1]) * 131) + p[0];
    }

}
