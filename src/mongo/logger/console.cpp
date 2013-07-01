/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/logger/console.h"

#include <iostream>

namespace mongo {
namespace {

    /*
     * Theory of operation:
     *
     * At process start, the loader initializes "consoleMutex" to NULL.  At some point during static
     * initialization, the static initialization process, running in the one and only extant thread,
     * allocates a new boost::mutex on the heap and assigns consoleMutex to point to it.  While
     * consoleMutex is still NULL, we know that there is only one thread extant, so it is safe to
     * skip locking the consoleMutex in the Console constructor.  Once the mutex is initialized,
     * users of Console can start acquiring it.
     */

    boost::mutex *consoleMutex = new boost::mutex;

}  // namespace

    Console::Console() : _consoleLock() {
        if (consoleMutex) {
            boost::unique_lock<boost::mutex> lk(*consoleMutex);
            lk.swap(_consoleLock);
        }
    }

    std::ostream& Console::out() { return std::cout; }
    std::istream& Console::in() { return std::cin; }

}  // namespace mongo
