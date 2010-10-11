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

#include <string>
#include <string.h>
#include <stdio.h>
#include <boost/shared_ptr.hpp>

#include "../inline_decls.h"
#include "../stringdata.h"

namespace mongo {

    /* Note the limit here is rather arbitrary and is simply a standard. generally the code works
       with any object that fits in ram.

       Also note that the server has some basic checks to enforce this limit but those checks are not exhaustive
       for example need to check for size too big after
         update $push (append) operation
         various db.eval() type operations
    */
    const int BSONObjMaxUserSize = 4 * 1024 * 1024;

    /*
       Sometimeswe we need objects slightly larger - an object in the replication local.oplog
       is slightly larger than a user object for example.
    */
    const int BSONObjMaxInternalSize = BSONObjMaxUserSize + ( 16 * 1024 );
    
    const int BufferMaxSize = 64 * 1024 * 1024;
    
    class StringBuilder;

    void msgasserted(int msgid, const char *msg);

    class BufBuilder {
    public:
        BufBuilder(int initsize = 512) : size(initsize) {
            if ( size > 0 ) {
                data = (char *) malloc(size);
                if( data == 0 )
                    msgasserted(10000, "out of memory BufBuilder");
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
        char* skip(int n) { return grow(n); }

        /* note this may be deallocated (realloced) if you keep writing. */
        char* buf() { return data; }
        const char* buf() const { return data; }

        /* assume ownership of the buffer - you must then free() it */
        void decouple() { data = 0; }

        void appendChar(char j){
            *((char*)grow(sizeof(char))) = j;
        }
        void appendNum(char j){
            *((char*)grow(sizeof(char))) = j;
        }
        void appendNum(short j) {
            *((short*)grow(sizeof(short))) = j;
        }
        void appendNum(int j) {
            *((int*)grow(sizeof(int))) = j;
        }
        void appendNum(unsigned j) {
            *((unsigned*)grow(sizeof(unsigned))) = j;
        }
        void appendNum(bool j) {
            *((bool*)grow(sizeof(bool))) = j;
        }
        void appendNum(double j) {
            *((double*)grow(sizeof(double))) = j;
        }
        void appendNum(long long j) {
            *((long long*)grow(sizeof(long long))) = j;
        }
        void appendNum(unsigned long long j) {
            *((unsigned long long*)grow(sizeof(unsigned long long))) = j;
        }

        void appendBuf(const void *src, size_t len) {
            memcpy(grow((int) len), src, len);
        }

        void appendStr(const StringData &str , bool includeEOO = true ) {
            const int len = str.size() + ( includeEOO ? 1 : 0 );
            memcpy(grow(len), str.data(), len);
        }

        int len() const {
            return l;
        }

        void setlen( int newLen ){
            l = newLen;
        }

        /* returns the pre-grow write position */
        inline char* grow(int by) {
            int oldlen = l;
            l += by;
            if ( l > size ) {
                grow_reallocate();
            }
            return data + oldlen;
        }

        int getSize() const { return size; }

    private:
        /* "slow" portion of 'grow()'  */
        void NOINLINE_DECL grow_reallocate(){
            int a = size * 2;
            if ( a == 0 )
                a = 512;
            if ( l > a )
                a = l + 16 * 1024;
            if ( a > BufferMaxSize )
                msgasserted(10000, "BufBuilder grow() > 64MB");
            data = (char *) realloc(data, a);
            size= a;
        }

        char *data;
        int l;
        int size;

        friend class StringBuilder;
    };

#if defined(_WIN32)
#pragma warning( disable : 4996 )
#endif

    class StringBuilder {
    public:
        StringBuilder( int initsize=256 )
            : _buf( initsize ){
        }

#define SBNUM(val,maxSize,macro) \
            int prev = _buf.l; \
            int z = sprintf( _buf.grow(maxSize) , macro , (val) );  \
            assert( z >= 0 ); \
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
#undef SBNUM

        void appendDoubleNice( double x ){
            int prev = _buf.l;
            char * start = _buf.grow( 32 );
            int z = sprintf( start , "%.16g" , x );
            assert( z >= 0 );
            _buf.l = prev + z;
            if( strchr(start, '.') == 0 && strchr(start, 'E') == 0 && strchr(start, 'N') == 0 ){
                write( ".0" , 2 );
            }
        }

        void write( const char* buf, int len){
            memcpy( _buf.grow( len ) , buf , len );
        }

        void append( const StringData& str ){
            memcpy( _buf.grow( str.size() ) , str.data() , str.size() );
        }
        StringBuilder& operator<<( const StringData& str ){
            append( str );
            return *this;
        }
        
        // access

        void reset( int maxSize = 0 ){
            _buf.reset( maxSize );
        }
        
        std::string str(){
            return std::string(_buf.data, _buf.l);
        }

    private:
        BufBuilder _buf;

        // non-copyable, non-assignable
        StringBuilder( const StringBuilder& );
        StringBuilder& operator=( const StringBuilder& );
    };

} // namespace mongo
