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
    const int BSONObjMaxUserSize = 16 * 1024 * 1024;

    /*
       Sometimeswe we need objects slightly larger - an object in the replication local.oplog
       is slightly larger than a user object for example.
    */
    const int BSONObjMaxInternalSize = BSONObjMaxUserSize + ( 16 * 1024 );

    const int BufferMaxSize = 64 * 1024 * 1024;

    class StringBuilder;

    void msgasserted(int msgid, const char *msg);

    class TrivialAllocator { 
    public:
        void* Malloc(size_t sz) { return malloc(sz); }
        void* Realloc(void *p, size_t sz) { return realloc(p, sz); }
        void Free(void *p) { free(p); }
    };

    class StackAllocator {
    public:
        enum { SZ = 512 };
        void* Malloc(size_t sz) {
            if( sz <= SZ ) return buf;
            return malloc(sz); 
        }
        void* Realloc(void *p, size_t sz) { 
            if( p == buf ) {
                if( sz <= SZ ) return buf;
                void *d = malloc(sz);
                if ( d == 0 )
                    msgasserted( 15912 , "out of memory StackAllocator::Realloc" );
                memcpy(d, p, SZ);
                return d;
            }
            return realloc(p, sz); 
        }
        void Free(void *p) { 
            if( p != buf )
                free(p); 
        }
    private:
        char buf[SZ];
    };

    template< class Allocator >
    class _BufBuilder {
        // non-copyable, non-assignable
        _BufBuilder( const _BufBuilder& );
        _BufBuilder& operator=( const _BufBuilder& );
        Allocator al;
    public:
        _BufBuilder(int initsize = 512) : size(initsize) {
            if ( size > 0 ) {
                data = (char *) al.Malloc(size);
                if( data == 0 )
                    msgasserted(10000, "out of memory BufBuilder");
            }
            else {
                data = 0;
            }
            l = 0;
        }
        ~_BufBuilder() { kill(); }

        void kill() {
            if ( data ) {
                al.Free(data);
                data = 0;
            }
        }

        void reset() {
            l = 0;
        }
        void reset( int maxSize ) {
            l = 0;
            if ( maxSize && size > maxSize ) {
                al.Free(data);
                data = (char*)al.Malloc(maxSize);
                if ( data == 0 )
                    msgasserted( 15913 , "out of memory BufBuilder::reset" );
                size = maxSize;
            }
        }

        /** leave room for some stuff later
            @return point to region that was skipped.  pointer may change later (on realloc), so for immediate use only
        */
        char* skip(int n) { return grow(n); }

        /* note this may be deallocated (realloced) if you keep writing. */
        char* buf() { return data; }
        const char* buf() const { return data; }

        /* assume ownership of the buffer - you must then free() it */
        void decouple() { data = 0; }

        void appendUChar(unsigned char j) {
            *((unsigned char*)grow(sizeof(unsigned char))) = j;
        }
        void appendChar(char j) {
            *((char*)grow(sizeof(char))) = j;
        }
        void appendNum(char j) {
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

        template<class T>
        void appendStruct(const T& s) {
            appendBuf(&s, sizeof(T));
        }

        void appendStr(const StringData &str , bool includeEndingNull = true ) {
            const int len = str.size() + ( includeEndingNull ? 1 : 0 );
            memcpy(grow(len), str.data(), len);
        }

        /** @return length of current string */
        int len() const { return l; }
        void setlen( int newLen ) { l = newLen; }
        /** @return size of the buffer */
        int getSize() const { return size; }

        /* returns the pre-grow write position */
        inline char* grow(int by) {
            int oldlen = l;
            l += by;
            if ( l > size ) {
                grow_reallocate();
            }
            return data + oldlen;
        }

    private:
        /* "slow" portion of 'grow()'  */
        void NOINLINE_DECL grow_reallocate() {
            int a = size * 2;
            if ( a == 0 )
                a = 512;
            if ( l > a )
                a = l + 16 * 1024;
            if ( a > BufferMaxSize )
                msgasserted(13548, "BufBuilder grow() > 64MB");
            data = (char *) al.Realloc(data, a);
            size= a;
        }

        char *data;
        int l;
        int size;

        friend class StringBuilder;
    };

    typedef _BufBuilder<TrivialAllocator> BufBuilder;

    /** The StackBufBuilder builds smaller datasets on the stack instead of using malloc.
          this can be significantly faster for small bufs.  However, you can not decouple() the 
          buffer with StackBufBuilder.
        While designed to be a variable on the stack, if you were to dynamically allocate one, 
          nothing bad would happen.  In fact in some circumstances this might make sense, say, 
          embedded in some other object.
    */
    class StackBufBuilder : public _BufBuilder<StackAllocator> { 
    public:
        StackBufBuilder() : _BufBuilder<StackAllocator>(StackAllocator::SZ) { }
        void decouple(); // not allowed. not implemented.
    };

#if defined(_WIN32)
#pragma warning( push )
// warning C4996: 'sprintf': This function or variable may be unsafe. Consider using sprintf_s instead. To disable deprecation, use _CRT_SECURE_NO_WARNINGS.
#pragma warning( disable : 4996 )
#endif

    /** stringstream deals with locale so this is a lot faster than std::stringstream for UTF8 */
    class StringBuilder {
    public:
        StringBuilder( int initsize=256 )
            : _buf( initsize ) {
        }

        StringBuilder& operator<<( double x ) {
            return SBNUM( x , 25 , "%g" );
        }
        StringBuilder& operator<<( int x ) {
            return SBNUM( x , 11 , "%d" );
        }
        StringBuilder& operator<<( unsigned x ) {
            return SBNUM( x , 11 , "%u" );
        }
        StringBuilder& operator<<( long x ) {
            return SBNUM( x , 22 , "%ld" );
        }
        StringBuilder& operator<<( unsigned long x ) {
            return SBNUM( x , 22 , "%lu" );
        }
        StringBuilder& operator<<( long long x ) {
            return SBNUM( x , 22 , "%lld" );
        }
        StringBuilder& operator<<( unsigned long long x ) {
            return SBNUM( x , 22 , "%llu" );
        }
        StringBuilder& operator<<( short x ) {
            return SBNUM( x , 8 , "%hd" );
        }
        StringBuilder& operator<<( char c ) {
            _buf.grow( 1 )[0] = c;
            return *this;
        }

        void appendDoubleNice( double x ) {
            int prev = _buf.l;
            char * start = _buf.grow( 32 );
            int z = sprintf( start , "%.16g" , x );
            assert( z >= 0 );
            _buf.l = prev + z;
            if( strchr(start, '.') == 0 && strchr(start, 'E') == 0 && strchr(start, 'N') == 0 ) {
                write( ".0" , 2 );
            }
        }

        void write( const char* buf, int len) { memcpy( _buf.grow( len ) , buf , len ); }

        void append( const StringData& str ) { memcpy( _buf.grow( str.size() ) , str.data() , str.size() ); }

        StringBuilder& operator<<( const StringData& str ) {
            append( str );
            return *this;
        }

        void reset( int maxSize = 0 ) { _buf.reset( maxSize ); }

        std::string str() const { return std::string(_buf.data, _buf.l); }
        
        int len() const { return _buf.l; }

    private:
        BufBuilder _buf;

        // non-copyable, non-assignable
        StringBuilder( const StringBuilder& );
        StringBuilder& operator=( const StringBuilder& );

        template <typename T>
        StringBuilder& SBNUM(T val,int maxSize,const char *macro)  {
            int prev = _buf.l;
            int z = sprintf( _buf.grow(maxSize) , macro , (val) );
            assert( z >= 0 );
            _buf.l = prev + z;
            return *this;
        }
    };

#if defined(_WIN32)
#pragma warning( pop )
#endif

} // namespace mongo
