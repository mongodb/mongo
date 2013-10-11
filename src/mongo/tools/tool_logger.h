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

#include "mongo/logger/logstream_builder.h"

namespace mongo {

    /**
     * This is a version of the Console class that uses stderr for output instead of stdout.  See
     * the description of the Console class for other details about how this class should operate
     */
    class ErrorConsole {
    public:
        ErrorConsole();

        std::ostream& out();

    private:
        boost::unique_lock<boost::mutex> _consoleLock;
    };

    using logger::LogstreamBuilder;

    /*
     * Informational messages.  Messages sent here will go to stdout normally, stderr if data is
     * being sent to stdout, and be silenced if the user specifies --quiet.
     */
    LogstreamBuilder toolInfoOutput();
    /*
     * Informational messages.  Messages sent here will go to stdout normally, stderr if data is
     * being sent to stdout, and be silenced if the user specifies --quiet.  Incudes extra log
     * decoration.
     */
    LogstreamBuilder toolInfoLog();
    /*
     * Error messages.  Messages sent here should always go to stderr and not be silenced by
     * --quiet.
     */
    LogstreamBuilder toolError();

}  // namespace mongo
