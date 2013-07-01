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

#ifndef _WIN32  // TODO(schwerin): Should be #if MONGO_CONFIG_HAVE_SYSLOG_H?

#include <boost/scoped_ptr.hpp>
#include <sstream>
#include <syslog.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"

namespace mongo {
namespace logger {

    /**
     * Appender for writing to syslog.  Users must have separately called openlog().
     */
    template <typename Event>
    class SyslogAppender : public Appender<Event> {
        MONGO_DISALLOW_COPYING(SyslogAppender);

    public:
        typedef Encoder<Event> EventEncoder;

        explicit SyslogAppender(EventEncoder* encoder) : _encoder(encoder) {}
        virtual Status append(const Event& event) {
            std::ostringstream os;
            _encoder->encode(event, os);
            if (!os)
                return Status(ErrorCodes::LogWriteFailed, "Error writing log message to syslog.");
            syslog(getSyslogPriority(event.getSeverity()), "%s", os.str().c_str());
            return Status::OK();
        }

    private:
        int getSyslogPriority(LogSeverity severity) {
            if (severity <= LogSeverity::Debug(1))
                return LOG_DEBUG;
            if (severity == LogSeverity::Warning())
                return LOG_WARNING;
            if (severity == LogSeverity::Error())
                return LOG_ERR;
            if (severity >= LogSeverity::Severe())
                return LOG_CRIT;
            // Info() and Log().
            return LOG_INFO;
        }
        boost::scoped_ptr<EventEncoder> _encoder;
    };

}  // namespace logger
}  // namespace mongo

#endif
