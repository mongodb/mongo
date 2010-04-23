/** @file undef_macros.h - remove mongo-specific macros that might cause issues */

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

// If you define a new global un-prefixed macro, please add it here and in redef_macros
// The push_macro and pop_macro pragmas as specific to MSVC and GCC

// #pragma once // this file is intended to be processed multiple times

#if !defined(MONGO_EXPOSE_MACROS) && !defined(MONGO_MACROS_CLEANED)

// util/allocator.h
#pragma push_macro("malloc")
#undef malloc
#pragma push_macro("realloc")
#undef realloc

// util/assert_util.h
#pragma push_macro("assert")
#undef assert
#pragma push_macro("dassert")
#undef dassert
#pragma push_macro("massert")
#undef massert
#pragma push_macro("uassert")
#undef uassert
#pragma push_macro("wassert")
#undef wassert
#pragma push_macro("xassert")
#undef xassert
#pragma push_macro("ASSERT_ID_DUPKEY")
#undef ASSERT_ID_DUPKEY
#pragma push_macro("ASSERT_STREAM_GOOD")
#undef ASSERT_STREAM_GOOD
#pragma push_macro("BOOST_CHECK_EXCEPTION")
#undef BOOST_CHECK_EXCEPTION
#pragma push_macro("DESTRUCTOR_GUARD")
#undef DESTRUCTOR_GUARD

// util/goodies.h
#pragma push_macro("PRINT")
#undef PRINT
#pragma push_macro("PRINTFL")
#undef PRINTFL
#pragma push_macro("asctime")
#undef asctime
#pragma push_macro("gmtime")
#undef gmtime
#pragma push_macro("localtime")
#undef localtime
#pragma push_macro("ctime")
#undef ctime

// util/log.h
#pragma push_macro("OUTPUT_ERRNOX")
#undef OUTPUT_ERRNOX
#pragma push_macro("OUTPUT_ERRNO")
#undef OUTPUT_ERRNO

// util/debug_util.h
#pragma push_macro("WIN")
#undef WIN
#pragma push_macro("DEV")
#undef DEV
#pragma push_macro("DEBUGGING")
#undef DEBUGGING
#pragma push_macro("SOMETIMES")
#undef SOMETIMES
#pragma push_macro("OCCASIONALLY")
#undef OCCASIONALLY
#pragma push_macro("RARELY")
#undef RARELY
#pragma push_macro("ONCE")
#undef ONCE

// db/instance.h
#pragma push_macro("OPREAD")
#undef OPREAD
#pragma push_macro("OPWRITE")
#undef OPWRITE

// stdafx.h
#pragma push_macro("null")
#undef null

#define MONGO_MACROS_CLEANED
#endif
