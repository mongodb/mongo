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

#include "../client/dbclient.h"

#ifdef malloc
# error malloc defined 0
#endif

#ifdef assert
# error assert defined 1
#endif

#include "../client/parallel.h" //uses assert

#ifdef assert
# error assert defined 2
#endif

#include "../client/redef_macros.h"

#ifndef assert
# error assert not defined 3
#endif

#include "../client/undef_macros.h"

#ifdef assert
# error assert defined 3
#endif


