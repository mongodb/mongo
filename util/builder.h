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

/* builder.h

*/

#pragma once

#include "../stdafx.h"

namespace mongo {

    class BufBuilder {
    public:
        BufBuilder(int initsize = 512) : size(initsize) {
            if ( size > 0 ) {
                data = (char *) malloc(size);
                assert(data);
            } else {
                data = 0;
            }
            l = 0;
        }
        ~BufBuilder() {
            kill();
        }

        void kill() {
            if ( data ) {
                free(data);
                data = 0;
            }
        }

        /* leave room for some stuff later */
        void skip(int n) {
            grow(n);
        }

        /* note this may be deallocated (realloced) if you keep writing. */
        char* buf() {
            return data;
        }

        /* assume ownership of the buffer - you must then free it */
        void decouple() {
            data = 0;
        }

        template<class T> void append(T j) {
            *((T*)grow(sizeof(T))) = j;
        }
        void append(short j) {
            append<short>(j);
        }
        void append(int j) {
            append<int>(j);
        }
        void append(unsigned j) {
            append<unsigned>(j);
        }
        void append(bool j) {
            append<bool>(j);
        }
        void append(double j) {
            append<double>(j);
        }

        void append(const void *src, int len) {
            memcpy(grow(len), src, len);
        }

        void append(const char *str) {
            append((void*) str, strlen(str)+1);
        }
        
        void append(const string &str) {
            append( (void *)str.c_str(), str.length() + 1 );
        }

        int len() const {
            return l;
        }

    private:
        /* returns the pre-grow write position */
        char* grow(int by) {
            int oldlen = l;
            l += by;
            if ( l > size ) {
                int a = size * 2;
                if ( a == 0 )
                    a = 512;
                if ( l > a )
                    a = l + 16 * 1024;
                assert( a < 64 * 1024 * 1024 );
                data = (char *) realloc(data, a);
                size= a;
            }
            return data + oldlen;
        }

        char *data;
        int l;
        int size;
    };

} // namespace mongo
