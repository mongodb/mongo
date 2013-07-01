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

namespace mongo {
namespace logger {

    LogSeverity LogSeverity::Severe() { return LogSeverity(-4); }
    LogSeverity LogSeverity::Error() { return LogSeverity(-3); }
    LogSeverity LogSeverity::Warning() { return LogSeverity(-2); }
    LogSeverity LogSeverity::Info() { return LogSeverity(-1); }
    LogSeverity LogSeverity::Log() { return LogSeverity(0); }
    LogSeverity LogSeverity::Debug(int debugLevel) { return LogSeverity(debugLevel); }

    LogSeverity LogSeverity::cast(int ll) { return LogSeverity(ll); }

    int LogSeverity::toInt() const { return _severity; }
    LogSeverity LogSeverity::moreSevere() const { return LogSeverity(_severity - 1); }
    LogSeverity LogSeverity::lessSevere() const { return LogSeverity(_severity + 1); }

    bool LogSeverity::operator==(LogSeverity other) const { return _severity == other._severity; }
    bool LogSeverity::operator!=(LogSeverity other) const { return _severity != other._severity; }
    bool LogSeverity::operator<(LogSeverity other) const { return _severity > other._severity; }
    bool LogSeverity::operator<=(LogSeverity other) const { return _severity >= other._severity; }
    bool LogSeverity::operator>(LogSeverity other) const { return _severity < other._severity; }
    bool LogSeverity::operator>=(LogSeverity other) const { return _severity <= other._severity; }

}  // namespace logger
}  // namespace mongo
