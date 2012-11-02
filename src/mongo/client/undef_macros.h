/** @file undef_macros.h remove mongo implementation macros after using */

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

#ifdef MONGO_MACROS_PUSHED

// util/allocator.h
#ifdef MONGO_MALLOC
#undef malloc
#pragma pop_macro("malloc")
#undef realloc
#pragma pop_macro("realloc")
#endif

// util/assert_util.h
#undef dassert
#pragma pop_macro("dassert")
#undef wassert
#pragma pop_macro("wassert")
#undef massert
#pragma pop_macro("massert")
#undef uassert
#pragma pop_macro("uassert")
#undef verify
#pragma pop_macro("verify")
#undef DESTRUCTOR_GUARD
#pragma pop_macro("DESTRUCTOR_GUARD")

// util/goodies.h
#undef PRINT
#pragma pop_macro("PRINT")
#undef PRINTFL
#pragma pop_macro("PRINTFL")

// util/debug_util.h
#undef DEV
#pragma pop_macro("DEV")
#undef DEBUGGING
#pragma pop_macro("DEBUGGING")
#undef SOMETIMES
#pragma pop_macro("SOMETIMES")
#undef OCCASIONALLY
#pragma pop_macro("OCCASIONALLY")
#undef RARELY
#pragma pop_macro("RARELY")
#undef ONCE
#pragma pop_macro("ONCE")

// util/log.h
#undef LOG
#pragma pop_macro("LOG")

#undef MONGO_MACROS_PUSHED
#endif
