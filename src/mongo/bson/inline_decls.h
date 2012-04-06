// inline_decls.h

/*    Copyright 2010 10gen Inc.
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
