// @file alignedbuilder.cpp

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

#include "pch.h"
#include "alignedbuilder.h"

namespace mongo {

    AlignedBuilder::AlignedBuilder(unsigned initSize) {
        _len = 0;
        _malloc(initSize);
        uassert(13584, "out of memory AlignedBuilder", _p._allocationAddress);
    }

    BOOST_STATIC_ASSERT(sizeof(void*) == sizeof(size_t));

    /** reset for a re-use. shrinks if > 128MB */
    void AlignedBuilder::reset() {
        _len = 0;
        RARELY {
            const unsigned sizeCap = 128*1024*1024;
            if (_p._size > sizeCap)
                _realloc(sizeCap, _len);
        }
    }

    /** reset with a hint as to the upcoming needed size specified */
    void AlignedBuilder::reset(unsigned sz) { 
        _len = 0;
        unsigned Q = 32 * 1024 * 1024 - 1;
        unsigned want = (sz+Q) & (~Q);
        if( _p._size == want ) {
            return;
        }        
        if( _p._size > want ) {
            if( _p._size <= 64 * 1024 * 1024 )
                return;
            bool downsize = false;
            RARELY { downsize = true; }
            if( !downsize )
                return;
        }
        _realloc(want, _len);
    }

    void AlignedBuilder::mallocSelfAligned(unsigned sz) {
        verify( sz == _p._size );
        void *p = malloc(sz + Alignment - 1);
        _p._allocationAddress = p;
        size_t s = (size_t) p;
        size_t sold = s;
        s += Alignment - 1;
        s = (s/Alignment)*Alignment;
        verify( s >= sold ); // begining
        verify( (s + sz) <= (sold + sz + Alignment - 1) ); //end
        _p._data = (char *) s;
    }

    /* "slow"/infrequent portion of 'grow()'  */
    void NOINLINE_DECL AlignedBuilder::growReallocate(unsigned oldLen) {
        dassert( _len > _p._size );
        unsigned a = _p._size;
        verify( a );
        while( 1 ) {
            if( a < 128 * 1024 * 1024 )
                a *= 2;
            else if( sizeof(int*) == 4 )
                a += 32 * 1024 * 1024;
            else 
                a += 64 * 1024 * 1024;
            DEV if( a > 256*1024*1024 ) { 
                log() << "dur AlignedBuilder too big, aborting in _DEBUG build" << endl;
                abort();
            }
            wassert( a <= 256*1024*1024 );
            verify( a <= 512*1024*1024 );
            if( _len < a )
                break;
        }
        _realloc(a, oldLen);
    }

    void AlignedBuilder::_malloc(unsigned sz) {
        _p._size = sz;
#if defined(_WIN32)
        void *p = VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        _p._allocationAddress = p;
        _p._data = (char *) p;
#elif defined(__linux__)
        // in theory #ifdef _POSIX_VERSION should work, but it doesn't on OS X 10.4, and needs to be testeed on solaris.
        // so for now, linux only for this.
        void *p = 0;
        int res = posix_memalign(&p, Alignment, sz);
        massert(13524, "out of memory AlignedBuilder", res == 0);
        _p._allocationAddress = p;
        _p._data = (char *) p;
#else
        mallocSelfAligned(sz);
        verify( ((size_t) _p._data) % Alignment == 0 );
#endif
    }

    void AlignedBuilder::_realloc(unsigned newSize, unsigned oldLen) {
        // posix_memalign alignment is not maintained on reallocs, so we can't use realloc().
        AllocationInfo old = _p;
        _malloc(newSize);
        verify( oldLen <= _len );
        memcpy(_p._data, old._data, oldLen);
        _free(old._allocationAddress);
    }

    void AlignedBuilder::_free(void *p) {
#if defined(_WIN32)
        VirtualFree(p, 0, MEM_RELEASE);
#else
        free(p);
#endif
    }

    void AlignedBuilder::kill() {
        _free(_p._allocationAddress);
        _p._allocationAddress = 0;
        _p._data = 0;
    }

}
