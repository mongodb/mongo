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

#include "mongo/otel/metrics/server_status_metric_name_validation.h"

#include "mongo/base/error_codes.h"

namespace mongo::otel::metrics {

namespace {

bool isAscii(StringData s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) > 127) {
            return false;
        }
    }
    return true;
}

bool isCamelCaseSegment(StringData seg) {
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

Status validateOneSegment(StringData seg) {
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

Status validateServerStatusMetricPath(StringData dottedPath) {
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

    size_t segStart = 0;
    for (size_t i = 0; i < dottedPath.size(); ++i) {
        if (dottedPath[i] != '.') {
            continue;
        }
        const StringData seg = dottedPath.substr(segStart, i - segStart);
        if (auto st = validateOneSegment(seg); !st.isOK()) {
            return st;
        }
        segStart = i + 1;
    }
    return validateOneSegment(dottedPath.substr(segStart, dottedPath.size() - segStart));
}

}  // namespace mongo::otel::metrics
