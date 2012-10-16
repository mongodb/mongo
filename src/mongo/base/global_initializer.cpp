/*    Copyright 2012 10gen Inc.
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

#include "mongo/base/global_initializer.h"

#include "mongo/base/initializer.h"

namespace mongo {

    Initializer& getGlobalInitializer() {
        static Initializer theGlobalInitializer;
        return theGlobalInitializer;
    }

namespace {

    // Make sure that getGlobalInitializer() is called at least once before main(), and so at least
    // once in a single-threaded context.  Otherwise, static initialization inside
    // getGlobalInitializer() won't be thread-safe.
    Initializer* _theGlobalInitializer = &getGlobalInitializer();

}  // namespace

}  // namespace mongo
