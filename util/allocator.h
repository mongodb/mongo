// allocator.h

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

#pragma once

#include "../stdafx.h"

namespace mongo {
    
    inline void * ourmalloc(size_t size) {
        void *x = malloc(size);
        if ( x == 0 ) dbexit( EXIT_OOM_MALLOC , "malloc fails");
        return x;
    }
    
    inline void * ourrealloc(void *ptr, size_t size) {
        void *x = realloc(ptr, size);
        if ( x == 0 ) dbexit( EXIT_OOM_REALLOC , "realloc fails");
        return x;
    }
    
#define malloc mongo::ourmalloc
#define realloc mongo::ourrealloc
    
#if defined(_WIN32)
    inline void our_debug_free(void *p) {
#if 0
        // this is not safe if you malloc < 4 bytes so we don't use anymore
        unsigned *u = (unsigned *) p;
        u[0] = 0xEEEEEEEE;
#endif
        free(p);
    }
#define free our_debug_free
#endif
    
} // namespace mongo
