/** @file redef_macros.h - redefine macros from undef_macros.h */

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
// The push_macro and pop_macro pragmas as specific to MSVC and GCC

// #pragma once // this file is intended to be processed multiple times

#if defined(MONGO_MACROS_CLEANED)

// util/allocator.h
#pragma pop_macro("malloc")
#pragma pop_macro("realloc")

// util/assert_util.h
#pragma pop_macro("assert")
#pragma pop_macro("dassert")
#pragma pop_macro("massert")
#pragma pop_macro("uassert")
#pragma pop_macro("wassert")
#pragma pop_macro("yassert")
#pragma pop_macro("xassert")
#pragma pop_macro("ASSERT_ID_DUPKEY")
#pragma pop_macro("ASSERT_STREAM_GOOD")
#pragma pop_macro("BOOST_CHECK_EXCEPTION")
#pragma pop_macro("DESTRUCTOR_GUARD")

// util/goodies.h
#pragma pop_macro("PRINT")
#pragma pop_macro("PRINTFL")
#pragma pop_macro("asctime")
#pragma pop_macro("gmtime")
#pragma pop_macro("localtime")
#pragma pop_macro("ctime")

// util/log.h
#pragma pop_macro("OUTPUT_ERRNOX")
#pragma pop_macro("OUTPUT_ERRNO")

// util/debug_util.h
#pragma pop_macro("WIN")
#pragma pop_macro("DEV")
#pragma pop_macro("DEBUGGING")
#pragma pop_macro("SOMETIMES")
#pragma pop_macro("OCCASIONALLY")
#pragma pop_macro("RARELY")
#pragma pop_macro("ONCE")

// db/instance.h
#pragma pop_macro("OPREAD")
#pragma pop_macro("OPWRITE")

// stdafx.h
#pragma pop_macro("null")

#undef MONGO_MACROS_CLEANED
#endif

