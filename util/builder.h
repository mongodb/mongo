/* builder.h */

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

        void setlen( int newLen ){
            l = newLen;
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
