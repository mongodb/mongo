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

#elif defined(_MSC_VER)

#define NOINLINE_DECL __declspec(noinline)

#else

#define NOINLINE_DECL

#endif


/* Note: do not clutter code with these -- ONLY use in hot spots / significant loops. */

//#if 1

//#if !defined(__GNUC__)

// branch prediction.  indicate we expect to enter the if statement body
#define MONGOIF(x) if( (x) )

// branch prediction.  indicate we expect to not enter the if statement body
#define MONGO_IF(x) if( (x) )

// prefetch data from memory
#define MONGOPREFETCH(x) { /*just check we compile:*/ sizeof(*x); }

#if 0

#define IF(x) if( __builtin_expect((x), 1) )

#define _IF(x) if( __builtin_expect((x), 0) )

#define PREFETCH(x) { /*just check we compile:*/ sizeof(*x); }

#endif
