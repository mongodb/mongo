/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/otel_metric_name_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/pcre.h"

#include <string_view>

namespace mongo::otel::metrics {

namespace {
// Each dot-separated segment starts with a lowercase letter and is then either:
//   - snake_case: only lowercase letters, digits, and underscores, OR
//   - camelCase:  only lowercase/uppercase letters and digits (no underscores)
// Mixing the two styles within a single segment is disallowed. Underscores in snake_case
// must be followed by at least one alphanumeric character, so trailing underscores and
// consecutive underscores are rejected.
constexpr std::string_view kSnakeCasePattern = "[a-z0-9]*(?:_[a-z0-9]+)*";
constexpr std::string_view kCamelCasePattern = "[a-zA-Z0-9]*";
std::string segmentPattern() {
    return fmt::format("[a-z](?:{0}|{1})", kSnakeCasePattern, kCamelCasePattern);
}

const pcre::Regex& nameRegex() {
    // Wrapping this in a function prevents static initialization order fiasco.
    static const pcre::Regex kNameRegex = []() {
        const auto seg = segmentPattern();
        return pcre::Regex{fmt::format("^{0}(?:\\.{0})*$", seg)};
    }();
    return kNameRegex;
}
}  // namespace

Status validateOtelMetricName(std::string_view name) {
    if (name.empty()) {
        return {ErrorCodes::InvalidOptions, "OpenTelemetry metric name cannot be empty"};
    }
    if (name.size() > kMaxOtelMetricNameLength) {
        return {ErrorCodes::InvalidOptions, "OpenTelemetry metric name exceeds maximum length"};
    }
    if (!nameRegex().matchView(name)) {
        return {ErrorCodes::InvalidOptions, "OpenTelemetry metric name is invalid"};
    }
    return Status::OK();
}

}  // namespace mongo::otel::metrics
