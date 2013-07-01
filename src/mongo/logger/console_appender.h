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

#include <boost/scoped_ptr.hpp>
#include <iostream>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/console.h"
#include "mongo/logger/encoder.h"

namespace mongo {
namespace logger {

    /**
     * Appender for writing to the console (stdout).
     */
    template <typename Event>
    class ConsoleAppender : public Appender<Event> {
        MONGO_DISALLOW_COPYING(ConsoleAppender);

    public:
        typedef Encoder<Event> EventEncoder;

        explicit ConsoleAppender(EventEncoder* encoder) : _encoder(encoder) {}
        virtual Status append(const Event& event) {
            Console console;
            _encoder->encode(event, console.out()).flush();
            if (!console.out())
                return Status(ErrorCodes::LogWriteFailed, "Error writing log message to console.");
            return Status::OK();
        }

    private:
        boost::scoped_ptr<EventEncoder> _encoder;
    };

}  // namespace logger
}  // namespace mongo
