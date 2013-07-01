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

#include "mongo/logger/log_severity.h"

#include <iostream>

namespace mongo {
namespace logger {

namespace {
    const char unknownSeverityString[] = "UNKNOWN";
    const char severeSeverityString[] = "SEVERE";
    const char errorSeverityString[] = "ERROR";
    const char warningSeverityString[] = "warning";
    const char infoSeverityString[] = "info";
    const char debugSeverityString[] = "debug";
}  // namespace

    StringData LogSeverity::toStringData() const {
        if (_severity > 0)
            return StringData(debugSeverityString, StringData::LiteralTag());
        if (*this == LogSeverity::Severe())
            return StringData(severeSeverityString, StringData::LiteralTag());
        if (*this == LogSeverity::Error())
            return StringData(errorSeverityString, StringData::LiteralTag());
        if (*this == LogSeverity::Warning())
            return StringData(warningSeverityString, StringData::LiteralTag());
        if (*this == LogSeverity::Info())
            return StringData(infoSeverityString, StringData::LiteralTag());
        if (*this == LogSeverity::Log())
            return StringData(infoSeverityString, StringData::LiteralTag());
        return StringData(unknownSeverityString, StringData::LiteralTag());
    }

    std::ostream& operator<<(std::ostream& os, LogSeverity severity) {
        return os << severity.toStringData();
    }

}  // namespace logger
}  // namespace mongo
