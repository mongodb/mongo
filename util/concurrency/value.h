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

namespace mongo {

    extern mutex _atomicMutex;

    /** atomic wrapper for a value.  enters a mutex on each access.  must
        be copyable.
    */
    template<typename T>
    class Atomic : boost::noncopyable {
        T val;
    public:
        Atomic<T>() { }

        void operator=(const T& a) {
            scoped_lock lk(_atomicMutex);
            val = a;
        }

        operator T() const {
            scoped_lock lk(_atomicMutex);
            return val;
        }

        /** example:
              Atomic<int> q;
              ...
              {
                Atomic<int>::tran t(q);
                if( q.ref() > 0 )
                    q.ref()--;
              }
        */
        class tran : private scoped_lock {
            Atomic<T>& _a;
        public:
            tran(Atomic<T>& a) : scoped_lock(_atomicMutex), _a(a) { }
            T& ref() { return _a.val; }
        };
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
