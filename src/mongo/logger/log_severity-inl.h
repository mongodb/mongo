/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

namespace mongo {
namespace logger {

LogSeverity LogSeverity::Severe() {
    return LogSeverity(-4);
}
LogSeverity LogSeverity::Error() {
    return LogSeverity(-3);
}
LogSeverity LogSeverity::Warning() {
    return LogSeverity(-2);
}
LogSeverity LogSeverity::Info() {
    return LogSeverity(-1);
}
LogSeverity LogSeverity::Log() {
    return LogSeverity(0);
}
LogSeverity LogSeverity::Debug(int debugLevel) {
    return LogSeverity(debugLevel);
}

LogSeverity LogSeverity::cast(int ll) {
    return LogSeverity(ll);
}

int LogSeverity::toInt() const {
    return _severity;
}
LogSeverity LogSeverity::moreSevere() const {
    return LogSeverity(_severity - 1);
}
LogSeverity LogSeverity::lessSevere() const {
    return LogSeverity(_severity + 1);
}

bool LogSeverity::operator==(LogSeverity other) const {
    return _severity == other._severity;
}
bool LogSeverity::operator!=(LogSeverity other) const {
    return _severity != other._severity;
}
bool LogSeverity::operator<(LogSeverity other) const {
    return _severity > other._severity;
}
bool LogSeverity::operator<=(LogSeverity other) const {
    return _severity >= other._severity;
}
bool LogSeverity::operator>(LogSeverity other) const {
    return _severity < other._severity;
}
bool LogSeverity::operator>=(LogSeverity other) const {
    return _severity <= other._severity;
}

}  // namespace logger
}  // namespace mongo
