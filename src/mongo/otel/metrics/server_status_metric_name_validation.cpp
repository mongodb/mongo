// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/server_status_metric_name_validation.h"

#include "mongo/base/error_codes.h"

#include <string_view>

namespace mongo::otel::metrics {

namespace {

bool isAscii(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) > 127) {
            return false;
        }
    }
    return true;
}

bool isCamelCaseSegment(std::string_view seg) {
    if (seg.empty()) {
        return false;
    }
    const char first = seg[0];
    if (first < 'a' || first > 'z') {
        return false;
    }
    for (size_t i = 1; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c >= 'a' && c <= 'z') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            continue;
        }
        return false;
    }
    return true;
}

Status validateOneSegment(std::string_view seg) {
    if (seg.empty()) {
        return {ErrorCodes::InvalidOptions,
                "serverStatus metric path cannot contain empty segments"};
    }
    if (!isCamelCaseSegment(seg)) {
        return {ErrorCodes::InvalidOptions, "serverStatus metric path contains an invalid segment"};
    }
    return Status::OK();
}

}  // namespace

Status validateServerStatusMetricPath(std::string_view dottedPath) {
    if (dottedPath.empty()) {
        return {ErrorCodes::InvalidOptions, "serverStatus metric path cannot be empty"};
    }
    if (!isAscii(dottedPath)) {
        return {ErrorCodes::InvalidOptions, "serverStatus metric path must be ASCII"};
    }
    if (dottedPath[0] == '.' || dottedPath[dottedPath.size() - 1] == '.') {
        return {ErrorCodes::InvalidOptions,
                "serverStatus metric path cannot start or end with a dot"};
    }
    if (dottedPath == "metrics" || dottedPath.starts_with("metrics.")) {
        return {ErrorCodes::InvalidOptions,
                "serverStatus metric path must not include the \"metrics\" prefix since "
                "the prefix is added automatically"};
    }

    size_t segStart = 0;
    for (size_t i = 0; i < dottedPath.size(); ++i) {
        if (dottedPath[i] != '.') {
            continue;
        }
        const std::string_view seg = dottedPath.substr(segStart, i - segStart);
        if (auto st = validateOneSegment(seg); !st.isOK()) {
            return st;
        }
        segStart = i + 1;
    }
    return validateOneSegment(dottedPath.substr(segStart, dottedPath.size() - segStart));
}

}  // namespace mongo::otel::metrics
