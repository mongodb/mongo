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

#include "mongo/base/string_data.h"
#include "mongo/logger/log_severity.h"
#include "mongo/platform/cstdint.h"

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
        MessageEventEphemeral(
                uint64_t curTimeMillis,
                LogSeverity severity,
                StringData contextName,
                StringData message) :
            _curTimeMillis(curTimeMillis),
            _severity(severity),
            _contextName(contextName),
            _message(message) {}

        uint64_t getCurTimeMillis() const { return _curTimeMillis; }
        LogSeverity getSeverity() const { return _severity; }
        StringData getContextName() const { return _contextName; }
        StringData getMessage() const { return _message; }

    private:
        uint64_t _curTimeMillis;
        LogSeverity _severity;
        StringData _contextName;
        StringData _message;
    };

}  // namespace logger
}  // namespace mongo
