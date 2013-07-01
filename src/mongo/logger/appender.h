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

#include "mongo/base/status.h"

namespace mongo {
namespace logger {

    /**
     * Interface for sinks in a logging system.  The core of logging is when events of type E are
     * appended to instances of Appender<E>.
     *
     * Example concrete instances are ConsoleAppender<E>, SyslogAppender<E> and
     * RotatableFileAppender<E>.
     */
    template <typename E>
    class Appender {
    public:
        typedef E Event;

        virtual ~Appender() {}

        /**
         * Appends "event", returns Status::OK() on success.
         */
        virtual Status append(const Event& event) = 0;
    };

}  // namespace logger
}  // namespace mongo
