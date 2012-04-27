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

#include <cfloat>
#include <string>
#include <sstream>
#include <iostream>
#include <string.h>
#include <stdio.h>
#include "../inline_decls.h"
#include "../stringdata.h"
#include "bswap.h"

namespace mongo {
    /* Accessing unaligned doubles on ARM generates an alignment trap and aborts with SIGBUS on Linux.
       Wrapping the double in a packed struct forces gcc to generate code that works with unaligned values too.
       The generated code for other architectures (which already allow unaligned accesses) is the same as if
       there was a direct pointer access.
       Should be replaced by little<double>, which does the same thing and more.
    */
    struct PackedDouble {
        double d;
    } PACKED_DECL;

    /* Note the limit here is rather arbitrary and is simply a standard. generally the code works
       with any object that fits in ram.

       Also note that the server has some basic checks to enforce this limit but those checks are not exhaustive
       for example need to check for size too big after
         update $push (append) operation
         various db.eval() type operations
    */
    const int BSONObjMaxUserSize = 16 * 1024 * 1024;

    /*
       Sometimes we need objects slightly larger - an object in the replication local.oplog
       is slightly larger than a user object for example.
    */
    const int BSONObjMaxInternalSize = BSONObjMaxUserSize + ( 16 * 1024 );

    const int BufferMaxSize = 64 * 1024 * 1024;

    void msgasserted(int msgid, const char *msg);

    template <typename Allocator>
    class StringBuilderImpl;

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

    private:
        /* 
         *  Wrap all primitive types in an endian/alignment insensitive way.
         */
        template<class T> void append( T j ) {
            little<T>::ref( grow( sizeof( T ) ) ) = j;
        }
    public:
        void appendUChar(unsigned char j) {
            append<unsigned char>( j );
        }
        void appendChar(char j) {
            append<char>( j );
        }
        void appendNum(char j) {
            append<char>( j );
        }
        void appendNum(short j) {
            append<short>( j );
        }
        void appendNum(int j) {
            append<int>( j );
        }
        void appendNum(unsigned j) {
            append<unsigned>( j );
        }
        void appendNum(bool j) {
            append<char>( j );
        }
        void appendNum(double j) {
            append<double>( j );
        }
        void appendNum(long long j) {
            append<long long>( j );
        }
        void appendNum(unsigned long long j) {
            append<unsigned long long>( j );
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
            int a = 64;
            while( a < l ) 
                a = a * 2;
            if ( a > BufferMaxSize ) {
                std::stringstream ss;
                ss << "BufBuilder attempted to grow() to " << a << " bytes, past the 64MB limit.";
                msgasserted(13548, ss.str().c_str());
            }
            data = (char *) al.Realloc(data, a);
            if ( data == NULL )
                msgasserted( 16070 , "out of memory BufBuilder::grow_reallocate" );
            size = a;
        }

        char *data;
        int l;
        int size;

        friend class StringBuilderImpl<Allocator>;
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

    namespace {
#if defined(_WIN32)
        int (*mongo_snprintf)(char *str, size_t size, const char *format, ...) = &sprintf_s;
#else
        int (*mongo_snprintf)(char *str, size_t size, const char *format, ...) = &snprintf;
#endif
    }

    /** stringstream deals with locale so this is a lot faster than std::stringstream for UTF8 */
    template <typename Allocator>
    class StringBuilderImpl {
    public:
        static const size_t MONGO_DBL_SIZE = 3 + DBL_MANT_DIG - DBL_MIN_EXP;
        static const size_t MONGO_S32_SIZE = 12;
        static const size_t MONGO_U32_SIZE = 11;
        static const size_t MONGO_S64_SIZE = 23;
        static const size_t MONGO_U64_SIZE = 22;
        static const size_t MONGO_S16_SIZE = 7;

        StringBuilderImpl() { }

        StringBuilderImpl& operator<<( double x ) {
            return SBNUM( x , MONGO_DBL_SIZE , "%g" );
        }
        StringBuilderImpl& operator<<( int x ) {
            return SBNUM( x , MONGO_S32_SIZE , "%d" );
        }
        StringBuilderImpl& operator<<( unsigned x ) {
            return SBNUM( x , MONGO_U32_SIZE , "%u" );
        }
        StringBuilderImpl& operator<<( long x ) {
            return SBNUM( x , MONGO_S64_SIZE , "%ld" );
        }
        StringBuilderImpl& operator<<( unsigned long x ) {
            return SBNUM( x , MONGO_U64_SIZE , "%lu" );
        }
        StringBuilderImpl& operator<<( long long x ) {
            return SBNUM( x , MONGO_S64_SIZE , "%lld" );
        }
        StringBuilderImpl& operator<<( unsigned long long x ) {
            return SBNUM( x , MONGO_U64_SIZE , "%llu" );
        }
        StringBuilderImpl& operator<<( short x ) {
            return SBNUM( x , MONGO_S16_SIZE , "%hd" );
        }
        StringBuilderImpl& operator<<( char c ) {
            _buf.grow( 1 )[0] = c;
            return *this;
        }

        void appendDoubleNice( double x ) {
            const int prev = _buf.l;
            const int maxSize = 32; 
            char * start = _buf.grow( maxSize );
            int z = mongo_snprintf( start , maxSize , "%.16g" , x );
            verify( z >= 0 );
            verify( z < maxSize );
            _buf.l = prev + z;
            if( strchr(start, '.') == 0 && strchr(start, 'E') == 0 && strchr(start, 'N') == 0 ) {
                write( ".0" , 2 );
            }
        }

        void write( const char* buf, int len) { memcpy( _buf.grow( len ) , buf , len ); }

        void append( const StringData& str ) { memcpy( _buf.grow( str.size() ) , str.data() , str.size() ); }

        StringBuilderImpl& operator<<( const StringData& str ) {
            append( str );
            return *this;
        }

        void reset( int maxSize = 0 ) { _buf.reset( maxSize ); }

        std::string str() const { return std::string(_buf.data, _buf.l); }
        
        int len() const { return _buf.l; }

    private:
        _BufBuilder<Allocator> _buf;

        // non-copyable, non-assignable
        StringBuilderImpl( const StringBuilderImpl& );
        StringBuilderImpl& operator=( const StringBuilderImpl& );

        template <typename T>
        StringBuilderImpl& SBNUM(T val,int maxSize,const char *macro)  {
            int prev = _buf.l;
            int z = mongo_snprintf( _buf.grow(maxSize) , maxSize , macro , (val) );
            verify( z >= 0 );
            verify( z < maxSize );
            _buf.l = prev + z;
            return *this;
        }
    };

    typedef StringBuilderImpl<TrivialAllocator> StringBuilder;
    typedef StringBuilderImpl<StackAllocator> StackStringBuilder;

} // namespace mongo
