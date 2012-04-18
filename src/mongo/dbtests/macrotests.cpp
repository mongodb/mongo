/*
 *    Copyright 2010 10gen Inc.
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

#undef MONGO_EXPOSE_MACROS

// pragma push_macro only works in gcc 4.3+
// However, you had to define a special macro
// and build gcc yourself for it to work in 4.3.
// Version 4.4+ activate the feature by default.

#define GCC_VERSION (__GNUC__ * 10000                 \
                     + __GNUC_MINOR__ * 100           \
                     + __GNUC_PATCHLEVEL__)

#if GCC_VERSION >= 40402

# define malloc 42

# include "mongo/client/redef_macros.h"
# include "mongo/client/undef_macros.h"

# if malloc == 42
# else
#  error malloc macro molested
# endif

# undef malloc

#endif // gcc 4.3

#include "mongo/client/dbclient.h"

#ifdef malloc
# error malloc macro defined
#endif

#ifdef verify
# error verify defined 1
#endif

#include "mongo/client/redef_macros.h"

#ifndef verify
# error verify not defined 3
#endif

#include "mongo/client/undef_macros.h"

#ifdef verify
# error verify defined 3
#endif


