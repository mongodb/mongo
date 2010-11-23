// @file bufreader.h parse a memory region into usable pieces

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
*/

#pragma once

namespace mongo { 

    /** helper to read and parse a block of memory 
        methods throw the eof exception if the operation would pass the end of th e
        buffer with which we are working.
    */
    class BufReader : boost::noncopyable { 
    public:
        class eof : public std::exception { 
        public:
            virtual const char * what() { return "BufReader eof"; }
        };

        BufReader(void *p, unsigned len) : _pos(p), _end(((char *)_pos)+len) { }

        /** prepare to read in the object specified, and advance buffer pointer */
        template <typename T>
        void read(T &t) { 
            t = *((T*) _pos);
            T *next = &t + 1;
            if( _end < next ) throw eof();
            _pos = next;
        }

        /** verify we can look at t, but do not advance */
        template <typename T>
        void peek(T &t) { 
            t = *((T*) _pos);
            const T *next = &t + 1;
            if( _end < next ) throw eof();
        }

        void advance(unsigned n) { _pos = ((char *) _pos) + n; }

    private:
        void *_pos;
        void *_end;
    };

    void example() { 
        BufReader r("abcdabcdabcd", 12);
        char x;
        int y;
        r.read(x); cout << x; // a
        r.read(y); cout << y; // 'bcda' as an int
    }

}
