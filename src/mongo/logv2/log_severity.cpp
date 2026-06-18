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

#include "mongo/logv2/log_severity.h"

#include <string_view>

namespace mongo::logv2 {
using namespace std::literals::string_view_literals;

namespace {

constexpr auto unknownSeverityString = "UNKNOWN"sv;
constexpr auto severeSeverityString = "SEVERE"sv;
constexpr auto errorSeverityString = "ERROR"sv;
constexpr auto warningSeverityString = "warning"sv;
constexpr auto infoSeverityString = "info"sv;
constexpr auto debugSeverityString = "debug"sv;

constexpr std::string_view kDebugLevelStrings[LogSeverity::kMaxDebugLevel] = {
    "D1"sv,
    "D2"sv,
    "D3"sv,
    "D4"sv,
    "D5"sv,
};

}  // namespace

std::string_view LogSeverity::toStringData() const {
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

std::string_view LogSeverity::toStringDataCompact() const {

    if ((*this == LogSeverity::Log()) || (*this == LogSeverity::Info()))
        return "I"sv;

    if ((_severity > 0) && (_severity <= kMaxDebugLevel))
        return kDebugLevelStrings[_severity - 1];

    if (*this == LogSeverity::Warning())
        return "W"sv;

    if (*this == LogSeverity::Error())
        return "E"sv;

    // 'S' might be confused with "Success"
    // Return 'F' to imply Fatal instead.
    if (*this == LogSeverity::Severe())
        return "F"sv;

    return "U"sv;
}

}  // namespace mongo::logv2
