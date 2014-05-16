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
