/** @file cleanup.h - remove mongo-specific macros that might cause issues
 *                    If included, it must be the last of the mongo headers
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

// If you define a new global un-prefixed macro, please undef it here

// util/allocator.h
#undef malloc
#undef realloc

// util/assert_util.h
#undef assert
#undef dassert
#undef massert
#undef uassert
#undef wassert
#undef yassert
#undef xassert
#undef ASSERT_ID_DUPKEY
#undef ASSERT_STREAM_GOOD
#undef BOOST_CHECK_EXCEPTION
#undef DESTRUCTOR_GUARD

// util/goodies.h
#undef PRINT
#undef PRINTFL
#undef asctime
#undef gmtime
#undef localtime
#undef ctime

// util/log.h
#undef OUTPUT_ERRNOX
#undef OUTPUT_ERRNO

// util/debug_util.h
#undef WIN
#undef DEV
#undef DEBUGGING
#undef SOMETIMES
#undef OCCASIONALLY
#undef RARELY
#undef ONCE

// db/instance.h
#undef OPREAD
#undef OPWRITE

// stdafx.h
#undef null

