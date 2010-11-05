// clientTest.cpp

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

/**
 * a copy of the simple test for the c++ driver, but including
 * fake, user versions of the most common std:: symbols, to see if
 * we allow them to interfere with our own headers.
 *
 * Note that the full std::qualification in the remainder of this code
 * is an artefact of the fact that it uses std:: types. Usually a user
 * would use eg, their own 'string' /in place of/ std::string. If they
 * tried to use them simultaneously, of course they'd have to fully
 * qualify them as is done here.
 */

#include "globalNamespacePoisoner.h"

#include "client/dbclient.h"

#include <iostream>

#ifndef assert
#  define assert(x) MONGO_assert(x)
#endif

using namespace std;
using namespace mongo;

int main() {
   // The point is only to compile

   return 0;
}
