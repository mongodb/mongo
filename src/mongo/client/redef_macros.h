/** @file redef_macros.h macros for mongo internals
    
    @see undef_macros.h undefines these after use to minimize name pollution.
*/

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

// If you define a new global un-prefixed macro, please add it here and in undef_macros

#define MONGO_MACROS_PUSHED 1

// util/allocator.h
#ifdef MONGO_MALLOC
#pragma push_macro("malloc")
#undef malloc
#define malloc MONGO_malloc
#pragma push_macro("realloc")
#undef realloc
#define realloc MONGO_realloc
#endif

// util/assert_util.h
#pragma push_macro("verify")
#undef verify
#define verify MONGO_verify
#pragma push_macro("dassert")
#undef dassert
#define dassert MONGO_dassert
#pragma push_macro("wassert")
#undef wassert
#define wassert MONGO_wassert
#pragma push_macro("massert")
#undef massert
#define massert MONGO_massert
#pragma push_macro("uassert")
#undef uassert
#define uassert MONGO_uassert
#pragma push_macro("DESTRUCTOR_GUARD")
#undef DESTRUCTOR_GUARD
#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD

// util/goodies.h
#pragma push_macro("PRINT")
#undef PRINT
#define PRINT MONGO_PRINT
#pragma push_macro("PRINTFL")
#undef PRINTFL
#define PRINTFL MONGO_PRINTFL

// util/debug_util.h
#pragma push_macro("DEV")
#undef DEV
#define DEV MONGO_DEV
#pragma push_macro("DEBUGGING")
#undef DEBUGGING
#define DEBUGGING MONGO_DEBUGGING
#pragma push_macro("SOMETIMES")
#undef SOMETIMES
#define SOMETIMES MONGO_SOMETIMES
#pragma push_macro("OCCASIONALLY")
#undef OCCASIONALLY
#define OCCASIONALLY MONGO_OCCASIONALLY
#pragma push_macro("RARELY")
#undef RARELY
#define RARELY MONGO_RARELY
#pragma push_macro("ONCE")
#undef ONCE
#define ONCE MONGO_ONCE

// util/log.h
#pragma push_macro("LOG")
#undef LOG
#define LOG MONGO_LOG


