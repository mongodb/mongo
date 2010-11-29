// @file alignedbuilder.h

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

#if defined(__sunos__)
#include <sys/mman.h>
#endif

namespace mongo { 

    /** a page-aligned BufBuilder. */
    class AlignedBuilder {
    public:
        AlignedBuilder(unsigned init_size) : _size(init_size) {
            _data = (char *) _malloc(_size);
            if( _data == 0 )
	        msgasserted(13523, "out of memory AlignedBuilder");
            _len = 0;
        }
        ~AlignedBuilder() { kill(); }

        /** reset for a re-use */
        void reset() { _len = 0; }

        /** leave room for some stuff later */
        char* skip(unsigned n) { return grow(n); }

        /** note this may be deallocated (realloced) if you keep writing or reset(). */
        const char* buf() const { return _data; }

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
            memcpy(grow((unsigned) len), src, len);
        }

        template<class T>
        void appendStruct(const T& s) { 
            appendBuf(&s, sizeof(T));
        }

        void appendStr(const StringData &str , bool includeEOO = true ) {
            const unsigned len = str.size() + ( includeEOO ? 1 : 0 );
            memcpy(grow(len), str.data(), len);
        }

        /** @return the in-use length */
        unsigned len() const { return _len; }

    private:
        /** returns the pre-grow write position */
        inline char* grow(unsigned by) {
            unsigned oldlen = _len;
            _len += by;
            if ( _len > _size ) {
                grow_reallocate();
            }
            return _data + oldlen;
        }

        /* "slow" portion of 'grow()'  */
        void NOINLINE_DECL grow_reallocate(){
            unsigned a = _size * 2;
            assert( a );
            if ( _len > a )
                a = _len + 16 * 1024;
            assert( a < 0x20000000 );
            _data = (char *) _realloc(_data, a, _len);
            _size = a;
        }

        static const unsigned Alignment = 8192;
        static void _free(void *p) { 
#if defined(_WIN32)
            VirtualFree(p, 0, MEM_RELEASE);
#else
            free(p);
#endif
        }
        static void* _malloc(unsigned sz) { 
#if defined(_WIN32)
            void *p = VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(_POSIX_VERSION)
            void *p = 0;
            int res = posix_memalign(&p, Alignment, sz);
            massert(13524, "out of memory AlignedBuilder", res == 0);
#else
            void *p = malloc(sz);
            assert( ((size_t) p) % Alignment == 0 );
#endif
	    return p;
        }
        static void* _realloc(void *ptr, unsigned newSize, unsigned oldSize) { 
            void *p = _malloc(newSize);
            memcpy(p, ptr, oldSize);
            _free(ptr);
            return p;
        }
        void kill() {
            _free(_data);
            _data = 0;
        }

        char *_data;
        unsigned _len;
        unsigned _size;
    };

}
