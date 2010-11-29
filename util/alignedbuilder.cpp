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

#if defined(_WIN32)
#include <windows.h>
#endif

#include "alignedbuilder.h"
#include "assert.h"
#include "../bson/inline_decls.h"

namespace mongo {

    AlignedBuilder::AlignedBuilder(unsigned init_size) : _size(init_size) {
        _data = (char *) _malloc(_size);
        if( _data == 0 )
            throw std::exception("out of memory AlignedBuilder");
        _len = 0;
    }

    void* AlignedBuilder::mallocSelfAligned(unsigned sz) {
        void *p = malloc(sz + Alignment - 1);
        _realAddr = p;
        size_t s = (size_t) p;
        s += Alignment - 1;
        s = (s/Alignment)*Alignment;
        return (void*) s;
    }

    /* "slow"/infrequent portion of 'grow()'  */
    void NOINLINE_DECL AlignedBuilder::grow_reallocate() {
        unsigned a = _size * 2;
        assert( a );
        if ( _len > a )
            a = _len + 16 * 1024;
        assert( a < 0x20000000 );
        _data = (char *) _realloc(_data, a, _len);
        _size = a;
    }

    void* AlignedBuilder::_malloc(unsigned sz) { 
#if defined(_WIN32)
        void *p = VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        _realAddr = p;
//#elif defined(_POSIX_VERSION)
// in theory _POSIX_VERSION should work, but it doesn't on OS X 10.4, and needs to be testeed on solaris.
// so for now, linux only for this.
#elif defined(__linux__)
        void *p = 0;
        int res = posix_memalign(&p, Alignment, sz);
        massert(13524, "out of memory AlignedBuilder", res == 0);
        _realAddr = p;
#else
        void *p = mallocSelfAligned(sz);
        assert( ((size_t) p) % Alignment == 0 );
#endif
        return p;
    }

    void* AlignedBuilder::_realloc(void *ptr, unsigned newSize, unsigned oldSize) { 
        // posix_memalign alignment is not maintained on reallocs
        void *oldAddr = _realAddr;
        void *p = _malloc(newSize);
        memcpy(p, ptr, oldSize);
        _free(oldAddr);
        return p;
    }

    void AlignedBuilder::_free(void *p) {
#if defined(_WIN32)
        VirtualFree(p, 0, MEM_RELEASE);
#else
        free(p);
#endif
    }

    void AlignedBuilder::kill() {
        _free(_realAddr);
        _data = 0;
        _realAddr = 0;
    }

}
