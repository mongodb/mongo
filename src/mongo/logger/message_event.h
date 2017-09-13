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

#pragma once

#include <cstdint>

#include "mongo/base/string_data.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/log_severity.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace logger {

/**
 * Free form text log message object that does not own the storage behind its message and
 * contextName.
 *
 * Used and owned by one thread.  This is the message type used by MessageLogDomain.
 */
class MessageEventEphemeral {
public:
    MessageEventEphemeral(Date_t date,
                          LogSeverity severity,
                          StringData contextName,
                          StringData message)
        : MessageEventEphemeral(date, severity, LogComponent::kDefault, contextName, message) {}

    MessageEventEphemeral(Date_t date,
                          LogSeverity severity,
                          LogComponent component,
                          StringData contextName,
                          StringData message)
        : _date(date),
          _severity(severity),
          _component(component),
          _contextName(contextName),
          _message(message) {}

    MessageEventEphemeral& setIsTruncatable(bool value) {
        _isTruncatable = value;
        return *this;
    }

    Date_t getDate() const {
        return _date;
    }
    LogSeverity getSeverity() const {
        return _severity;
    }
    LogComponent getComponent() const {
        return _component;
    }
    StringData getContextName() const {
        return _contextName;
    }
    StringData getMessage() const {
        return _message;
    }
    bool isTruncatable() const {
        return _isTruncatable;
    }

private:
    Date_t _date;
    LogSeverity _severity;
    LogComponent _component;
    StringData _contextName;
    StringData _message;
    bool _isTruncatable = true;
};

}  // namespace logger
}  // namespace mongo
