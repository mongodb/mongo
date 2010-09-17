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

// #pragma once // this file is intended to be processed multiple times

#if defined(MONGO_MACROS_CLEANED)

// util/allocator.h
#define malloc MONGO_malloc
#define realloc MONGO_realloc

// util/assert_util.h
#define assert MONGO_assert
#define dassert MONGO_dassert
#define wassert MONGO_wassert
#define massert MONGO_massert
#define uassert MONGO_uassert
#define BOOST_CHECK_EXCEPTION MONGO_BOOST_CHECK_EXCEPTION
#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD

// util/goodies.h
#define PRINT MONGO_PRINT
#define PRINTFL MONGO_PRINTFL
#define asctime MONGO_asctime
#define gmtime MONGO_gmtime
#define localtime MONGO_localtime
#define ctime MONGO_ctime

// util/debug_util.h
#define DEV MONGO_DEV
#define DEBUGGING MONGO_DEBUGGING
#define SOMETIMES MONGO_SOMETIMES
#define OCCASIONALLY MONGO_OCCASIONALLY
#define RARELY MONGO_RARELY
#define ONCE MONGO_ONCE

// util/log.h
#define LOG MONGO_LOG

#undef MONGO_MACROS_CLEANED
#endif

