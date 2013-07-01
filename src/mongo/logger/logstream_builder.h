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
#include <sstream>
#include <string>

#include "mongo/logger/labeled_level.h"
#include "mongo/logger/log_severity.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/util/exit_code.h"

namespace mongo {
namespace logger {

    class Tee;

    /**
     * Stream-ish object used to build and append log messages.
     */
    class LogstreamBuilder {
    public:
        static LogSeverity severityCast(int ll) { return LogSeverity::cast(ll); }
        static LogSeverity severityCast(LogSeverity ls) { return ls; }
        static LabeledLevel severityCast(const LabeledLevel &labeled) { return labeled; }

        /**
         * Construct a LogstreamBuilder that writes to "domain" on destruction.
         *
         * "contextName" is a short name of the thread or other context.
         * "severity" is the logging priority/severity of the message.
         */
        LogstreamBuilder(MessageLogDomain* domain,
                         const std::string& contextName,
                         LogSeverity severity);

        /**
         * Deprecated.
         */
        LogstreamBuilder(MessageLogDomain* domain,
                         const std::string& contextName,
                         LabeledLevel labeledLevel);

        /**
         * Copies a LogstreamBuilder.  LogstreamBuilder instances are copyable only until the first
         * call to stream() or operator<<.
         *
         * TODO(schwerin): After C++11 transition, replace with a move-constructor, and make
         * LogstreamBuilder movable.
         */
        LogstreamBuilder(const LogstreamBuilder& other);

        /**
         * Destroys a LogstreamBuilder().  If anything was written to it via stream() or operator<<,
         * constructs a MessageLogDomain::Event and appends it to the associated domain.
         */
        ~LogstreamBuilder();


        /**
         * Sets an optional prefix for the message.
         */
        LogstreamBuilder& setBaseMessage(const std::string& baseMessage) {
            _baseMessage = baseMessage;
            return *this;
        }

        std::ostream& stream() { makeStream(); return *_os; }

        LogstreamBuilder& operator<<(const char *x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(const std::string& x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(const StringData& x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(char *x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(char x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(int x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(ExitCode x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(long x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(unsigned long x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(unsigned x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(unsigned short x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(double x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(void *x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(const void *x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(long long x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(unsigned long long x) { stream() << x; return *this; }
        LogstreamBuilder& operator<<(bool x) { stream() << x; return *this; }

        template <typename T>
        LogstreamBuilder& operator<<(const T& x) {
            stream() << x.toString();
            return *this;
        }

        LogstreamBuilder& operator<< (std::ostream& ( *manip )(std::ostream&)) {
            stream() << manip;
            return *this;
        }
        LogstreamBuilder& operator<< (std::ios_base& (*manip)(std::ios_base&)) {
            stream() << manip;
            return *this;
        }

        /**
         * In addition to appending the message to _domain, write it to the given tee.  May only
         * be called once per instance of LogstreamBuilder.
         */
        void operator<<(Tee* tee);

    private:
        LogstreamBuilder& operator=(const LogstreamBuilder& other);

        void makeStream();

        MessageLogDomain* _domain;
        std::string _contextName;
        LogSeverity _severity;
        std::string _baseMessage;
        std::ostringstream* _os;
        Tee* _tee;

    };


}  // namespace logger
}  // namespace mongo
