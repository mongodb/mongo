// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
