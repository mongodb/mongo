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
#include <string.h>

namespace mongo {

    class StringBuilder;

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

        void reset( int maxSize = 0 ){
            l = 0;
            if ( maxSize && size > maxSize ){
                free(data);
                data = (char*)malloc(maxSize);
                size = maxSize;
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

        friend class StringBuilder;
    };

    class StringBuilder {
    public:
        StringBuilder( int initsize=256 )
            : _buf( initsize ){
        }

#define SBNUM(val,maxSize,macro) \
            int prev = _buf.l; \
            int z = sprintf( _buf.grow(maxSize) , macro , (val) );  \
            _buf.l = prev + z; \
            return *this; 
        

        StringBuilder& operator<<( double x ){
            SBNUM( x , 25 , "%g" );
        }
        StringBuilder& operator<<( int x ){
            SBNUM( x , 11 , "%d" );
        }
        StringBuilder& operator<<( unsigned x ){
            SBNUM( x , 11 , "%u" );
        }
        StringBuilder& operator<<( long x ){
            SBNUM( x , 22 , "%ld" );
        }
        StringBuilder& operator<<( unsigned long x ){
            SBNUM( x , 22 , "%lu" );
        }
        StringBuilder& operator<<( long long x ){
            SBNUM( x , 22 , "%lld" );
        }
        StringBuilder& operator<<( unsigned long long x ){
            SBNUM( x , 22 , "%llu" );
        }
        StringBuilder& operator<<( short x ){
            SBNUM( x , 8 , "%hd" );
        }
        StringBuilder& operator<<( char c ){
            _buf.grow( 1 )[0] = c;
            return *this;
        }

        void append( const char * str ){
            int x = strlen( str );
            memcpy( _buf.grow( x ) , str , x );
        }
        StringBuilder& operator<<( const char * str ){
            append( str );
            return *this;
        }
        StringBuilder& operator<<( const string& s ){
            append( s.c_str() );
            return *this;
        }
        
        // access

        void reset( int maxSize = 0 ){
            _buf.reset( maxSize );
        }
        
        string str(){
            return string(_buf.data,0,_buf.l);
        }

    private:
        BufBuilder _buf;
    };

} // namespace mongo
