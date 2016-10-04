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

#include "mongo/logger/log_severity.h"

#include <iostream>

namespace mongo {
namespace logger {

namespace {
constexpr auto unknownSeverityString = "UNKNOWN"_sd;
constexpr auto severeSeverityString = "SEVERE"_sd;
constexpr auto errorSeverityString = "ERROR"_sd;
constexpr auto warningSeverityString = "warning"_sd;
constexpr auto infoSeverityString = "info"_sd;
constexpr auto debugSeverityString = "debug"_sd;
}  // namespace

StringData LogSeverity::toStringData() const {
    if (_severity > 0)
        return debugSeverityString;
    if (*this == LogSeverity::Severe())
        return severeSeverityString;
    if (*this == LogSeverity::Error())
        return errorSeverityString;
    if (*this == LogSeverity::Warning())
        return warningSeverityString;
    if (*this == LogSeverity::Info())
        return infoSeverityString;
    if (*this == LogSeverity::Log())
        return infoSeverityString;
    return unknownSeverityString;
}

char LogSeverity::toChar() const {
    if (_severity > 0)
        return 'D';
    // 'S' might be confused with "Success"
    // Return 'F' to imply Fatal instead.
    if (*this == LogSeverity::Severe())
        return 'F';
    if (*this == LogSeverity::Error())
        return 'E';
    if (*this == LogSeverity::Warning())
        return 'W';
    if (*this == LogSeverity::Info())
        return 'I';
    if (*this == LogSeverity::Log())
        return 'I';
    // Should not reach here - returning 'U' for Unknown severity
    // to be consistent with toStringData().
    return 'U';
}

std::ostream& operator<<(std::ostream& os, LogSeverity severity) {
    return os << severity.toStringData();
}

}  // namespace logger
}  // namespace mongo
