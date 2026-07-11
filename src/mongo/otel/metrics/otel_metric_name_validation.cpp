// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
