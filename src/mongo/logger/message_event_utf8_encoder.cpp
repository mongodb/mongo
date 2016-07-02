/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/logger/message_event_utf8_encoder.h"

#include <iostream>

#include "mongo/util/time_support.h"

namespace mongo {
namespace logger {

static MessageEventDetailsEncoder::DateFormatter _dateFormatter = outputDateAsISOStringLocal;

void MessageEventDetailsEncoder::setDateFormatter(DateFormatter dateFormatter) {
    _dateFormatter = dateFormatter;
}

MessageEventDetailsEncoder::DateFormatter MessageEventDetailsEncoder::getDateFormatter() {
    return _dateFormatter;
}

MessageEventDetailsEncoder::~MessageEventDetailsEncoder() {}
std::ostream& MessageEventDetailsEncoder::encode(const MessageEventEphemeral& event,
                                                 std::ostream& os) {
    static const size_t maxLogLine = 10 * 1024;

    _dateFormatter(os, event.getDate());
    os << ' ';

    os << event.getSeverity().toChar();
    os << ' ';

    LogComponent component = event.getComponent();
    os << component;
    os << ' ';

    StringData contextName = event.getContextName();
    if (!contextName.empty()) {
        os << '[' << contextName << "] ";
    }

    StringData msg = event.getMessage();
    if (event.isTruncatable() && msg.size() > maxLogLine) {
        os << "warning: log line attempted (" << msg.size() / 1024 << "kB) over max size ("
           << maxLogLine / 1024 << "kB), printing beginning and end ... ";
        os << msg.substr(0, maxLogLine / 3);
        os << " .......... ";
        os << msg.substr(msg.size() - (maxLogLine / 3));
    } else {
        os << msg;
    }
    if (!msg.endsWith(StringData("\n"_sd)))
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
