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

// #pragma once // this file is intended to be processed multiple times


/** MONGO_EXPOSE_MACROS - when defined, indicates that you are compiling a mongo program rather
                          than just using the C++ driver.
*/
#if !defined(MONGO_EXPOSE_MACROS) && !defined(MONGO_MACROS_CLEANED)

// util/allocator.h
#undef malloc
#undef realloc

// util/assert_util.h
#undef assert
#undef dassert
#undef wassert
#undef massert
#undef uassert
#undef BOOST_CHECK_EXCEPTION
#undef DESTRUCTOR_GUARD

// util/goodies.h
#undef PRINT
#undef PRINTFL
#undef asctime
#undef gmtime
#undef localtime
#undef ctime

// util/debug_util.h
#undef DEV
#undef DEBUGGING
#undef SOMETIMES
#undef OCCASIONALLY
#undef RARELY
#undef ONCE

// util/log.h
#undef LOG

#define MONGO_MACROS_CLEANED
#endif
