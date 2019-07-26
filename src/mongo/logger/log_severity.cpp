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

constexpr StringData kDebugLevelStrings[LogSeverity::kMaxDebugLevel] = {
    "D1"_sd,
    "D2"_sd,
    "D3"_sd,
    "D4"_sd,
    "D5"_sd,
};

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

StringData LogSeverity::toStringDataCompact() const {

    if ((*this == LogSeverity::Log()) || (*this == LogSeverity::Info()))
        return "I "_sd;

    if ((_severity > 0) && (_severity <= kMaxDebugLevel))
        return kDebugLevelStrings[_severity - 1];

    if (*this == LogSeverity::Warning())
        return "W "_sd;

    if (*this == LogSeverity::Error())
        return "E "_sd;

    // 'S' might be confused with "Success"
    // Return 'F' to imply Fatal instead.
    if (*this == LogSeverity::Severe())
        return "F "_sd;

    return "U "_sd;
}

std::ostream& operator<<(std::ostream& os, LogSeverity severity) {
    return os << severity.toStringData();
}

}  // namespace logger
}  // namespace mongo
