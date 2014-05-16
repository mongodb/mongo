// inline_decls.h

/*    Copyright 2010 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#if defined(__GNUC__)

#define NOINLINE_DECL __attribute__((noinline))
#define PACKED_DECL __attribute__((packed))

#elif defined(_MSC_VER)

#define NOINLINE_DECL __declspec(noinline)
#define PACKED_DECL

#else

#define NOINLINE_DECL
#define PACKED_DECL

#endif

namespace mongo {

/* Note: do not clutter code with these -- ONLY use in hot spots / significant loops. */

#if !defined(__GNUC__)

// branch prediction.  indicate we expect to be true
# define MONGO_likely(x) ((bool)(x))

// branch prediction.  indicate we expect to be false
# define MONGO_unlikely(x) ((bool)(x))

# if defined(_WIN32)
    // prefetch data from memory
    inline void prefetch(const void *p) { 
#if defined(_MM_HINT_T0)
        _mm_prefetch((char *) p, _MM_HINT_T0);
#endif
    }
#else
    inline void prefetch(void *p) { }
#endif

#else

# define MONGO_likely(x) ( __builtin_expect((bool)(x), 1) )
# define MONGO_unlikely(x) ( __builtin_expect((bool)(x), 0) )

    inline void prefetch(void *p) { 
        __builtin_prefetch(p);
    }

#endif

}
