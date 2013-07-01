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

#pragma once

#include <boost/thread/mutex.hpp>
#include <iosfwd>

namespace mongo {

    /**
     * Representation of the console.  Use this in place of cout/cin, in applications that write to
     * the console from multiple threads (such as those that use the logging subsystem).
     *
     * The Console type is synchronized such that only one instance may be in the fully constructed
     * state at a time.  Correct usage is to instantiate one, write or read from it as desired, and
     * then destroy it.
     *
     * The console streams accept UTF-8 encoded data, and attempt to write it to the attached
     * console faithfully.
     *
     * TODO(schwerin): If no console is attached on Windows (services), should writes here go to the
     * event logger?
     */
    class Console {
    public:
        Console();

        std::ostream& out();
        std::istream& in();

    private:
        boost::unique_lock<boost::mutex> _consoleLock;
    };

}  // namespace mongo
