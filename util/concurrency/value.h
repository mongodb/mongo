/* @file value.h
   concurrency helpers Atomic<T> and DiagStr
*/

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "mutex.h"

namespace mongo {

    /** declare that a variable that is "guarded" by a mutex.

        The decl documents the rule.  For example "counta and countb are guarded by xyzMutex":

          Guarded<int, xyzMutex> counta;
          Guarded<int, xyzMutex> countb;

        Upon use, specify the scoped_lock object.  This makes it hard for someone 
        later to forget to be in the lock.  Check is made that it is the right lock in _DEBUG
        builds at runtime.
    */
    template <typename T, mutex& BY>
    class Guarded {
        T _val;
    public:
        T& ref(const scoped_lock& lk) {
            dassert( lk._mut == &BY );
            return _val;
        }
    };

    /** this string COULD be mangled but with the double buffering, assuming writes
    are infrequent, it's unlikely.  thus, this is reasonable for lockless setting of
    diagnostic strings, where their content isn't critical.
    */
    class DiagStr {
        char buf1[256];
        char buf2[256];
        char *p;
    public:
        DiagStr() {
            memset(buf1, 0, 256);
            memset(buf2, 0, 256);
            p = buf1;
        }

        const char * get() const { return p; }

        void set(const char *s) {
            char *q = (p==buf1) ? buf2 : buf1;
            strncpy(q, s, 255);
            p = q;
        }
    };

}
