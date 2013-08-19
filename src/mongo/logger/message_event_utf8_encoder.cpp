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

#include "mongo/logger/message_event_utf8_encoder.h"

#include <iostream>

#include "mongo/util/time_support.h"

namespace mongo {
namespace logger {

    static MessageEventDetailsEncoder::DateFormatter _dateFormatter = dateToISOStringLocal;

    void MessageEventDetailsEncoder::setDateFormatter(DateFormatter dateFormatter) {
        _dateFormatter = dateFormatter;
    }

    MessageEventDetailsEncoder::DateFormatter MessageEventDetailsEncoder::getDateFormatter() {
        return _dateFormatter;
    }

    MessageEventDetailsEncoder::~MessageEventDetailsEncoder() {}
    std::ostream& MessageEventDetailsEncoder::encode(const MessageEventEphemeral& event,
                                                     std::ostream &os) {

        static const size_t maxLogLine = 10 * 1024;

        os << _dateFormatter(event.getDate()) << ' ';
        StringData contextName = event.getContextName();
        if (!contextName.empty()) {
            os << '[' << contextName << "] ";
        }

        LogSeverity severity = event.getSeverity();
        if (severity >= LogSeverity::Info()) {
            os << severity << ": ";
        }

        StringData msg = event.getMessage();
        if (msg.size() > maxLogLine) {
            os << "warning: log line attempted (" << msg.size() / 1024 << "k) over max size (" <<
                maxLogLine / 1024 << "k), printing beginning and end ... ";
            os << msg.substr(0, maxLogLine / 3);
            os << " .......... ";
            os << msg.substr(msg.size() - (maxLogLine / 3));
        }
        else {
            os << msg;
        }
        if (!msg.endsWith(StringData("\n", StringData::LiteralTag())))
            os << '\n';
        return os;
    }

    MessageEventWithContextEncoder::~MessageEventWithContextEncoder() {}
    std::ostream& MessageEventWithContextEncoder::encode(const MessageEventEphemeral& event,
                                                         std::ostream& os) {
        StringData contextName = event.getContextName();
        if (!contextName.empty()) {
            os << '[' << contextName << "] ";
        }
        StringData message = event.getMessage();
        os << message;
        if (!message.endsWith("\n"))
            os << '\n';
        return os;
    }

    MessageEventUnadornedEncoder::~MessageEventUnadornedEncoder() {}
    std::ostream& MessageEventUnadornedEncoder::encode(const MessageEventEphemeral& event,
                                                       std::ostream& os) {
        StringData message = event.getMessage();
        os << message;
        if (!message.endsWith("\n"))
            os << '\n';
        return os;
    }

}  // namespace logger
}  // namespace mongo
